#!/usr/bin/env python3
"""mutate.py -- SLM-001's benchmark-corpus generator (§16.3 item 5).

docs/archive/ARCHITECTURE_OVERHAUL.md §16.3 item 5 names the shape this
tool has to take, quoting DESIGN_NOTES.md §6's dataset plan almost verbatim:
"a code mutator takes passing test files, intentionally breaks them
mechanically (delete a semicolon, swap `int` -> `string`, drop a closing
brace, swap argument order), runs them through the real compiler to capture
the exact error + context lines". §16.3 is explicit that building this corpus
is independent of, and comes BEFORE, any fine-tuning plan (D9, §27.2) --
this script and run_benchmark.py are the whole of that "build it first" step.

What this does, concretely: for each (source .tr test, line, mutation kind)
tuple in MUTATIONS below, apply the mutation to a REAL, currently-PASSING
test from tests/, compile the mutated source with the real compiler (no
--smart) to capture the actual diagnostic it raises, and write the result to
corpus.json. run_benchmark.py then re-compiles every corpus entry WITH
--smart and reports the §16.4 metrics table.

Deliberately NOT a generic pattern-scanning auto-mutator: the (file, line,
kind) tuples below are hand-picked so each one reliably produces a REAL
compile-time diagnostic (not, say, a swapped-argument mutation that still
type-checks and just silently computes the wrong runtime value -- confirmed
live while building this corpus that `math::power(2, 3)` -> `math::power(3,
2)` is exactly that trap: still (int, int), still compiles, just wrong at
runtime, which is useless as a DIAGNOSTIC benchmark case). A future session
growing the corpus should verify a candidate mutation the same way: run it
through `build/turfc` with NO --smart first and confirm it actually fails to
compile before adding it here.

Usage:
    python3 scripts/slm/slm_benchmark/mutate.py
Writes: scripts/slm/slm_benchmark/corpus.json (checked into the repo -- the
mutated SOURCE lives inside the JSON, not as loose .tr files, so the corpus
is one self-contained artifact).
"""
from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
TURF = REPO_ROOT / "build" / "turfc"
CORPUS_PATH = Path(__file__).resolve().parent / "corpus.json"


def delete_semicolon(lines: list[str], line: int, extra: dict) -> list[str]:
    """Drop the FIRST trailing ';' on a 1-based `line` -- SEM-004/AGENTS.md's
    "definite assignment" story means a missing ';' is almost always a hard
    syntax error, not a silent behavior change, which is exactly what makes
    it a useful, unambiguous benchmark case."""
    idx = line - 1
    lines[idx] = lines[idx].replace(";", "", 1)
    return lines


def swap_type(lines: list[str], line: int, extra: dict) -> list[str]:
    """Replace `extra["from"]` with `extra["to"]` in a declaration's type
    annotation on a 1-based `line` -- produces a genuine TYPE_MISMATCH
    (SEM-002's literal-representability/assignment-compatibility checks)."""
    idx = line - 1
    old, new = extra["from"], extra["to"]
    pattern = re.compile(r"(?<![A-Za-z0-9_])" + re.escape(old) + r"(?![A-Za-z0-9_])")
    lines[idx] = pattern.sub(new, lines[idx], count=1)
    return lines


def drop_closing_brace(lines: list[str], line: int, extra: dict) -> list[str]:
    """Delete the entire 1-based `line`, which must be a lone '}' -- produces
    a structural parse error (unbalanced braces)."""
    idx = line - 1
    assert lines[idx].strip() == "}", f"line {line} is not a lone '}}': {lines[idx]!r}"
    del lines[idx]
    return lines


def typo_identifier(lines: list[str], line: int, extra: dict) -> list[str]:
    """Replace `extra["name"]` with a single-transposition typo on a 1-based
    `line` -- produces an UNDECLARED_VAR-shaped error with exactly one
    unambiguous Damerau-Levenshtein match, i.e. SLM-001 item 1's own
    deterministic-fix target."""
    idx = line - 1
    name = extra["name"]
    assert len(name) >= 2, "need at least 2 chars to transpose"
    typo = name[1] + name[0] + name[2:]  # swap the first two characters
    # A transposed name must still be a valid IDENTIFIER (letter/underscore
    # first) -- swapping "p1"'s two characters produces "1p", which the
    # LEXER (not the resolver) rejects as a malformed number token instead,
    # producing an unrelated cascade of parse errors rather than the clean
    # UNDECLARED_VAR shape this mutation is meant to exercise. Caught live
    # while building this corpus -- see this function's own doc comment.
    assert typo[0].isalpha() or typo[0] == "_", (
        f"transposing {name!r} -> {typo!r} is not a valid identifier "
        "(starts with a digit) -- pick a different `name`")
    pattern = re.compile(r"(?<![A-Za-z0-9_])" + re.escape(name) + r"(?![A-Za-z0-9_])")
    lines[idx] = pattern.sub(typo, lines[idx], count=1)
    return lines


MUTATORS = {
    "delete_semicolon": delete_semicolon,
    "swap_type": swap_type,
    "drop_closing_brace": drop_closing_brace,
    "typo_identifier": typo_identifier,
}

# Each entry: a real, currently-passing test under tests/, the 1-based line
# to mutate, the mutation kind, and any kind-specific `extra` data. Verified
# individually (see this file's own doc comment) to actually fail to compile
# once mutated -- confirmed again mechanically by this script itself, which
# refuses to add a case to corpus.json unless the mutated file's exit code
# is non-zero.
MUTATIONS = [
    {
        "id": "semicolon_var_decl",
        "source_test": "tests/01_syntax/02_semicolon.tr",
        "kind": "delete_semicolon",
        "line": 4,
        "extra": {},
    },
    {
        "id": "type_mismatch_int_to_string",
        "source_test": "tests/01_syntax/02_semicolon.tr",
        "kind": "swap_type",
        "line": 4,
        "extra": {"from": "int", "to": "string"},
    },
    {
        "id": "unbalanced_brace_struct",
        "source_test": "tests/01_syntax/04_pratt_lvalue.tr",
        "kind": "drop_closing_brace",
        "line": 5,
        "extra": {},
    },
    {
        "id": "typo_field_access",
        "source_test": "tests/01_syntax/04_pratt_lvalue.tr",
        "kind": "typo_identifier",
        "line": 13,
        "extra": {"name": "val"},
    },
    {
        "id": "type_mismatch_double_to_int",
        "source_test": "tests/03_typechecker/01_arithmetic_basic.tr",
        "kind": "swap_type",
        "line": 21,
        "extra": {"from": "double", "to": "int"},
    },
    {
        "id": "typo_undeclared_variable",
        "source_test": "tests/03_typechecker/01_arithmetic_basic.tr",
        "kind": "typo_identifier",
        "line": 47,
        "extra": {"name": "abs1"},
    },
]


def compile_capture(source: str) -> tuple[int, str]:
    """Writes `source` to a scratch file and compiles it (no --smart).
    Returns (exit_code, combined stdout+stderr)."""
    scratch = Path("/tmp") / "turf_slm_benchmark_scratch.tr"
    scratch.write_text(source)
    proc = subprocess.run(
        [str(TURF), str(scratch)],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
    )
    return proc.returncode, proc.stdout + proc.stderr


def strip_ansi(s: str) -> str:
    return re.sub(r"\033\[[0-9;]*m", "", s)


def main() -> int:
    if not TURF.exists():
        print(f"error: {TURF} missing -- build the compiler first", file=sys.stderr)
        return 1

    corpus = []
    for spec in MUTATIONS:
        source_path = REPO_ROOT / spec["source_test"]
        lines = source_path.read_text().splitlines(keepends=True)
        mutated_lines = MUTATORS[spec["kind"]](list(lines), spec["line"], spec["extra"])
        mutated_source = "".join(mutated_lines)

        code, output = compile_capture(mutated_source)
        clean_output = strip_ansi(output)
        if code == 0:
            print(f"SKIPPING {spec['id']}: mutation did not break compilation "
                  f"(exit 0) -- fix the (file, line, kind) tuple in MUTATIONS.",
                  file=sys.stderr)
            continue

        code_match = re.search(r"\[E(\d+)\]", clean_output)
        corpus.append({
            "id": spec["id"],
            "source_test": spec["source_test"],
            "kind": spec["kind"],
            "mutated_source": mutated_source,
            "baseline_exit_code": code,
            "baseline_error_code": f"E{code_match.group(1)}" if code_match else "",
            "baseline_output": clean_output,
        })
        print(f"OK  {spec['id']} ({spec['kind']} on {spec['source_test']}:{spec['line']})"
              f" -> {corpus[-1]['baseline_error_code'] or '(no E-code)'}")

    CORPUS_PATH.write_text(json.dumps(corpus, indent=2) + "\n")
    print(f"\nWrote {len(corpus)} case(s) to {CORPUS_PATH.relative_to(REPO_ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
