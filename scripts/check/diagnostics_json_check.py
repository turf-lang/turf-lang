#!/usr/bin/env python3
"""DX-2 and IF-1: turfc's JSON outputs against their published schemas.

Checks every tests/**/fail_*.tr (not tests/99_known_bugs, tests/custom or
tests/compat) with `turfc <file> --check --diagnostics=json`, run from the
file's own directory, and fails when

  - a line of stderr is not one JSON object,
  - an object does not match docs/reference/schemas/diagnostics.schema.json,
  - the JSON form reports a different number of diagnostics, or different
    codes in a different order, than the text form of the same compile,
  - the exit status differs between the two forms,
  - a fixture under docs/reference/schemas/diagnostics-fixtures/ differs from
    what the compiler prints for its source (IF-2: the files a consumer's
    tests read). `--write` regenerates them.
  - `turfc --version --json` (IF-1) does not match
    docs/reference/schemas/version.schema.json, disagrees with the text
    `--version` lines (release, interface), or names a diagnostics schema
    version other than the one diagnostics carry.

The schema is read from the published file and checked with the small subset
of JSON Schema it uses (type, enum, pattern, required, properties, items,
minItems, minimum, oneOf, $ref), so this needs no third-party package.

Usage: scripts/check/diagnostics_json_check.py [--compiler build/turfc]
                                              [--jobs N] [--write]
"""

import argparse
import concurrent.futures
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCHEMA = ROOT / "docs/reference/schemas/diagnostics.schema.json"
VERSION_SCHEMA = ROOT / "docs/reference/schemas/version.schema.json"
FIXTURES = ROOT / "docs/reference/schemas/diagnostics-fixtures"
ANSI = re.compile(r"\x1b\[[0-9;]*m")
HEADER = re.compile(r"^── (error|warning)\b[^\[]*(?:\[(E\d{3}))?")
TYPES = {"object": dict, "array": list, "string": str, "boolean": bool,
         "null": type(None)}


def validate(value, schema, root, where="$"):
    """The first mismatch between `value` and `schema`, or None."""
    if "$ref" in schema:
        name = schema["$ref"].split("/")[-1]
        return validate(value, root["$defs"][name], root, where)
    if "oneOf" in schema:
        matches = [s for s in schema["oneOf"] if validate(value, s, root, where) is None]
        return None if len(matches) == 1 else f"{where}: matches {len(matches)} of oneOf"
    if "enum" in schema and value not in schema["enum"]:
        return f"{where}: {value!r} not in {schema['enum']}"
    if "type" in schema:
        kinds = schema["type"] if isinstance(schema["type"], list) else [schema["type"]]
        ok = any(isinstance(value, int) and not isinstance(value, bool)
                 if k == "integer" else isinstance(value, TYPES[k]) and
                 not (k != "boolean" and isinstance(value, bool))
                 for k in kinds)
        if not ok:
            return f"{where}: {value!r} is not {kinds}"
    if isinstance(value, str) and "pattern" in schema and not re.search(schema["pattern"], value):
        return f"{where}: {value!r} does not match {schema['pattern']}"
    if isinstance(value, int) and "minimum" in schema and value < schema["minimum"]:
        return f"{where}: {value} < {schema['minimum']}"
    if isinstance(value, dict):
        for key in schema.get("required", []):
            if key not in value:
                return f"{where}: missing '{key}'"
        for key, sub in schema.get("properties", {}).items():
            if key in value:
                err = validate(value[key], sub, root, f"{where}.{key}")
                if err:
                    return err
    if isinstance(value, list):
        if len(value) < schema.get("minItems", 0):
            return f"{where}: fewer than {schema['minItems']} items"
        for i, item in enumerate(value):
            err = validate(item, schema.get("items", {}), root, f"{where}[{i}]")
            if err:
                return err
    return None


def run(compiler, path, json_form):
    env = dict(os.environ)
    env.pop("TURF_HOME", None)
    args = [compiler, path.name, "--check"]
    if json_form:
        args.append("--diagnostics=json")
    out = subprocess.run(args, cwd=path.parent, env=env, capture_output=True,
                         text=True, timeout=120)
    return out.returncode, out.stderr


def check_file(compiler, schema, path):
    """Problems with one file, as strings."""
    problems = []
    status_json, err_json = run(compiler, path, True)
    status_text, err_text = run(compiler, path, False)
    if status_json != status_text:
        problems.append(f"exit status {status_json} in JSON, {status_text} in text")
    objects = []
    for n, line in enumerate(err_json.splitlines(), 1):
        try:
            obj = json.loads(line)
        except json.JSONDecodeError:
            problems.append(f"stderr line {n} is not JSON: {line[:80]!r}")
            continue
        err = validate(obj, schema, schema)
        if err:
            problems.append(f"stderr line {n}: {err}")
        objects.append(obj)
    text = [(m.group(1), m.group(2)) for m in
            (HEADER.match(l) for l in ANSI.sub("", err_text).splitlines()) if m]
    got = [(o.get("severity"), o.get("code")) for o in objects]
    if got != text:
        problems.append(f"JSON reports {got}, text reports {text}")
    return path, problems


def fixture_output(compiler, source):
    """What the fixture for `source` should hold: the JSON lines, as run from
    the repository root on the path relative to it."""
    env = dict(os.environ)
    env.pop("TURF_HOME", None)
    out = subprocess.run([compiler, str(source.relative_to(ROOT)), "--check",
                          "--diagnostics=json"], cwd=ROOT, env=env,
                         capture_output=True, text=True, timeout=120)
    return out.stderr


def check_manifest(compiler):
    """Problems with `turfc --version --json`, as strings."""
    env = dict(os.environ)
    env.pop("TURF_HOME", None)
    out = subprocess.run([compiler, "--version", "--json"], env=env,
                         capture_output=True, text=True, timeout=60)
    try:
        manifest = json.loads(out.stdout)
    except json.JSONDecodeError:
        return [f"--version --json is not one JSON object: {out.stdout[:80]!r}"]
    schema = json.loads(VERSION_SCHEMA.read_text())
    problems = []
    err = validate(manifest, schema, schema)
    if err:
        problems.append(f"--version --json: {err}")
        return problems
    lines = subprocess.run([compiler, "--version"], env=env, capture_output=True,
                           text=True, timeout=60).stdout.splitlines()
    want = f"interface {manifest['interface']['major']}.{manifest['interface']['minor']}"
    if lines[:2] != [f"turfc {manifest['turfc']}", want]:
        problems.append(f"--version says {lines[:2]}, --version --json says "
                        f"turfc {manifest['turfc']}, {want}")
    sample = (FIXTURES / "error_with_fix.jsonl").read_text().splitlines()[0]
    if json.loads(sample)["version"] != manifest["schemas"]["diagnostics"]:
        problems.append("the manifest's diagnostics schema version is not the "
                        "version the diagnostics carry")
    if "diagnostics-json" not in manifest["features"]:
        problems.append("the manifest does not list diagnostics-json")
    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", default=str(ROOT / "build/turfc"))
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    ap.add_argument("--write", action="store_true",
                    help="regenerate the fixtures instead of comparing them")
    args = ap.parse_args()
    compiler = str(Path(args.compiler).resolve())
    schema = json.loads(SCHEMA.read_text())

    failed = 0
    if not args.write:
        for p in check_manifest(compiler):
            print(f"FAIL {p}")
            failed += 1
    sources = json.loads((FIXTURES / "sources.json").read_text())
    for name, src in sources.items():
        want = fixture_output(compiler, ROOT / src)
        fixture = FIXTURES / f"{name}.jsonl"
        if args.write:
            fixture.write_text(want)
            print(f"wrote {fixture.relative_to(ROOT)}")
        elif not fixture.exists() or fixture.read_text() != want:
            print(f"FAIL {fixture.relative_to(ROOT)}: differs from the output for {src} "
                  "(regenerate with --write if the change is meant)")
            failed += 1
    if args.write:
        return 0

    tests = sorted(p for p in (ROOT / "tests").rglob("fail_*.tr")
                   if not {"99_known_bugs", "custom", "compat"} & set(p.relative_to(ROOT / "tests").parts))
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for path, problems in pool.map(lambda p: check_file(compiler, schema, p), tests):
            for p in problems:
                print(f"FAIL {path.relative_to(ROOT)}: {p}")
            failed += bool(problems)
    print(f"diagnostics_json_check: the --version --json manifest, {len(tests)} "
          f"fail_*.tr files, {len(sources)} fixtures, {failed} failing")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
