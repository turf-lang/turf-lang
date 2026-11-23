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
  - `turfc --unsafe-report` (LB-2), over a small project and over files, does
    not match docs/reference/schemas/unsafe-report.schema.json.
  - a package graph (IF-2) does not match its schema: the fixtures under
    docs/reference/schemas/package-graph-fixtures/ (package-graph.schema.json
    and, for `turf graph --unsafe`, package-graph-report.schema.json), turfc
    does not build and test the fixture project through them, turfc and the
    schema disagree on a graph with an unknown package key, or (when build/turf
    exists) what `turf graph` prints for examples/plotter does not match.
  - `turfc --registry-json` (IF-3) does not match
    docs/reference/schemas/diagnostic-registry.schema.json, lists a code
    twice, gives a code that is not retired no explanation, or differs from
    the four entries in docs/reference/schemas/diagnostic-registry-sample.json
    (`--write` regenerates it).

The schema is read from the published file and checked with the small subset
of JSON Schema it uses (type, enum, const, pattern, required, properties,
additionalProperties (a schema or false), items, minItems, minimum, oneOf,
$ref), so this needs no
third-party package.

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
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCHEMA = ROOT / "docs/reference/schemas/diagnostics.schema.json"
VERSION_SCHEMA = ROOT / "docs/reference/schemas/version.schema.json"
UNSAFE_SCHEMA = ROOT / "docs/reference/schemas/unsafe-report.schema.json"
GRAPH_SCHEMA = ROOT / "docs/reference/schemas/package-graph.schema.json"
REPORT_SCHEMA = ROOT / "docs/reference/schemas/package-graph-report.schema.json"
GRAPH_FIXTURES = ROOT / "docs/reference/schemas/package-graph-fixtures"
REGISTRY_SCHEMA = ROOT / "docs/reference/schemas/diagnostic-registry.schema.json"
REGISTRY_SAMPLE = ROOT / "docs/reference/schemas/diagnostic-registry-sample.json"
# An error with its own page, a warning at warn, one at allow, a retired code.
REGISTRY_SAMPLE_CODES = ["E041", "E165", "E188", "E013"]
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
    if "const" in schema and value != schema["const"]:
        return f"{where}: {value!r} is not {schema['const']!r}"
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
        extra = schema.get("additionalProperties")
        if extra is False:
            for key in value:
                if key not in schema.get("properties", {}):
                    return f"{where}: unknown key '{key}'"
        if isinstance(extra, dict):
            for key, sub in value.items():
                if key not in schema.get("properties", {}):
                    err = validate(sub, extra, root, f"{where}.{key}")
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
    if "unsafe-report" not in manifest["features"]:
        problems.append("the manifest does not list unsafe-report")
    return problems


def check_unsafe_report(compiler):
    """`turfc --unsafe-report` against its schema, both forms (LB-2)."""
    schema = json.loads(UNSAFE_SCHEMA.read_text())
    problems = []
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "app/src").mkdir(parents=True)
        (root / "ffi/src").mkdir(parents=True)
        (root / "app/src/main.tr").write_text("fn main(): int {\n    return 0;\n}\n")
        (root / "ffi/src/ffi.tr").write_text(
            'unsafe module;\nextern from "z" fn zlibVersion(): string;\n'
            'pub fn f(): int {\n    return unsafe { 1; };\n}\n'
            'pub fn g(): string {\n    return "open;\n}\n')
        (root / "graph.json").write_text(json.dumps({
            "format": 1, "root": "app", "packages": {
                "app": {"src": "app/src", "deps": ["ffi"], "allow_unsafe": True},
                "ffi": {"src": "ffi/src", "deps": []}}}))
        runs = {
            "--package-graph": [compiler, "--unsafe-report", "--package-graph", "graph.json"],
            "files": [compiler, "--unsafe-report", "ffi/src/ffi.tr", "app/src/main.tr"],
        }
        for name, args in runs.items():
            out = subprocess.run(args, cwd=root, capture_output=True, text=True, timeout=120)
            if out.returncode != 0:
                problems.append(f"--unsafe-report ({name}): exit {out.returncode}: {out.stderr.strip()}")
                continue
            try:
                report = json.loads(out.stdout)
            except json.JSONDecodeError as e:
                problems.append(f"--unsafe-report ({name}): not JSON: {e}")
                continue
            err = validate(report, schema, schema)
            if err:
                problems.append(f"--unsafe-report ({name}): {err}")
            elif name == "files" and "error" not in report["files"][0]:
                problems.append("--unsafe-report: a file that does not lex has no \"error\"")
    return problems


def check_package_graphs(compiler):
    """IF-2: the package graph's schemas, fixtures and producers."""
    graph = json.loads(GRAPH_SCHEMA.read_text())
    report = json.loads(REPORT_SCHEMA.read_text())
    problems = []
    for name, schema in (("graph.json", graph), ("build-graph.json", graph),
                         ("report.json", report)):
        err = validate(json.loads((GRAPH_FIXTURES / name).read_text()), schema, schema)
        if err:
            problems.append(f"package-graph-fixtures/{name}: {err}")
    env = dict(os.environ)
    env.pop("TURF_HOME", None)
    runs = {
        "build": [compiler, "app/src/main.tr", "--package-graph", "build-graph.json", "--check"],
        "test": [compiler, "--test", "app/tests", "--package-graph", "graph.json"],
    }
    for name, args in runs.items():
        out = subprocess.run(args, cwd=GRAPH_FIXTURES, env=env, capture_output=True,
                             text=True, timeout=300)
        if out.returncode != 0:
            problems.append(f"package-graph-fixtures ({name}): exit {out.returncode}: "
                            f"{(out.stdout + out.stderr).strip()[-400:]}")
    # The schema and turfc reject the same thing: an unknown package key.
    with tempfile.TemporaryDirectory() as tmp:
        bad = json.loads((GRAPH_FIXTURES / "build-graph.json").read_text())
        bad["packages"]["app"]["alow_unsafe"] = True
        for pkg in bad["packages"].values():
            pkg["src"] = str(GRAPH_FIXTURES / pkg["src"])
        (Path(tmp) / "bad.json").write_text(json.dumps(bad))
        out = subprocess.run([compiler, str(GRAPH_FIXTURES / "app/src/main.tr"),
                              "--package-graph", str(Path(tmp) / "bad.json"), "--check"],
                             env=env, capture_output=True, text=True, timeout=120)
        if out.returncode == 0:
            problems.append("turfc accepts a package graph with an unknown package key")
        if validate(bad, graph, graph) is None:
            problems.append("package-graph.schema.json accepts an unknown package key")
    # The producer: turf graph, when this build has the front end.
    turf = Path(compiler).parent / "turf"
    if turf.exists():
        for flags, schema, label in (([], graph, "turf graph"),
                                     (["--unsafe"], report, "turf graph --unsafe")):
            out = subprocess.run([str(turf), "graph", "--offline", *flags],
                                 cwd=ROOT / "examples/plotter", env=env,
                                 capture_output=True, text=True, timeout=300)
            if out.returncode != 0:
                problems.append(f"{label}: exit {out.returncode}: {out.stderr.strip()}")
                continue
            try:
                err = validate(json.loads(out.stdout), schema, schema)
            except json.JSONDecodeError as e:
                err = f"not JSON: {e}"
            if err:
                problems.append(f"{label} (examples/plotter): {err}")
    return problems


def registry(compiler):
    env = dict(os.environ)
    env.pop("TURF_HOME", None)
    out = subprocess.run([compiler, "--registry-json"], env=env, capture_output=True,
                         text=True, timeout=120)
    if out.returncode != 0:
        raise RuntimeError(f"exit {out.returncode}: {out.stderr.strip()}")
    return json.loads(out.stdout)


def registry_sample(reg):
    by_code = {c["code"]: c for c in reg["codes"]}
    return json.dumps({"format": reg["format"],
                       "codes": [by_code[c] for c in REGISTRY_SAMPLE_CODES]},
                      indent=1, ensure_ascii=False) + "\n"


def check_registry(compiler):
    """IF-3: the registry the release archive carries."""
    schema = json.loads(REGISTRY_SCHEMA.read_text())
    try:
        reg = registry(compiler)
    except (RuntimeError, json.JSONDecodeError) as e:
        return [f"--registry-json: {e}"]
    problems = []
    err = validate(reg, schema, schema)
    if err:
        problems.append(f"--registry-json: {err}")
        return problems
    seen = set()
    for c in reg["codes"]:
        if c["code"] in seen:
            problems.append(f"--registry-json: {c['code']} is listed twice")
        seen.add(c["code"])
        if not c["retired"] and not c["explanation"]:
            problems.append(f"--registry-json: {c['code']} has no explanation")
    if not REGISTRY_SAMPLE.exists() or REGISTRY_SAMPLE.read_text() != registry_sample(reg):
        problems.append(f"{REGISTRY_SAMPLE.relative_to(ROOT)} differs from --registry-json "
                        "(regenerate with --write if the change is meant)")
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
        for p in (check_manifest(compiler) + check_unsafe_report(compiler) +
                  check_package_graphs(compiler) + check_registry(compiler)):
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
        REGISTRY_SAMPLE.write_text(registry_sample(registry(compiler)))
        print(f"wrote {REGISTRY_SAMPLE.relative_to(ROOT)}")
        return 0

    tests = sorted(p for p in (ROOT / "tests").rglob("fail_*.tr")
                   if not {"99_known_bugs", "custom", "compat"} & set(p.relative_to(ROOT / "tests").parts))
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for path, problems in pool.map(lambda p: check_file(compiler, schema, p), tests):
            for p in problems:
                print(f"FAIL {path.relative_to(ROOT)}: {p}")
            failed += bool(problems)
    print(f"diagnostics_json_check: the --version --json manifest, the package graph, {len(tests)} "
          f"fail_*.tr files, {len(sources)} fixtures, {failed} failing")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
