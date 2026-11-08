#!/usr/bin/env python3
"""build_corpus.py -- the `--smart` benchmark corpus (SL-3, TDP-2-008).

Takes passing, single-file programs (tests/ and showcase/), applies ONE
single-line mutation, compiles the mutant with the real compiler, and keeps it
only if the compiler rejects it with a compile-time diagnostic near the
mutated line. Each case's oracle is the original program: a fix is correct
when the fixed program prints what the original printed and exits 0 (its own
test::assert calls still hold).

Beside those single mutations it adds:
  - two-error cases: two accepted mutants of one file, far apart, that the
    compiler reports as two or more errors (kind `multi:<a>+<b>`);
  - one case for every error code a fail_*.tr test reports alone that no
    mutant reached (kind `fail_test`, oracle `recompile`: the fixed program
    compiles, with no error the original lacked);
  - hard negatives from hard_negatives/*.tr, where the right answer is no
    suggestion (oracle `abstain`): a fix needs a decision the compiler cannot
    make.

Cases are split into train and heldout BY SOURCE FILE (sha1 of the path), so
no held-out case has a sibling mutant of the same file in train. Prompt and
weight tuning read train only; check_heldout.py enforces it.

Usage (from anywhere):
    python3 scripts/slm/slm_benchmark/build_corpus.py [--jobs 2] [--seed 20260]

Writes corpus/train.jsonl, corpus/heldout.jsonl and corpus/STATS.md next to
this file. Stdlib only. Scratch files go to $CORPUS_WORK (default: a temp
directory). Needs the compiler at build/turfc (not rebuilt here).
"""
from __future__ import annotations

import argparse
import collections
import concurrent.futures as cf
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
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
TURFC = REPO / "build" / "turfc"
OUT = HERE / "corpus"
HARD = HERE / "hard_negatives"
STD = REPO / "lib" / "std"

TIMEOUT = 10
SKIP_DIRS = ("tests/custom/", "tests/99_known_bugs/", "tests/compat/", "tests/api/")
SKIP_IMPORTS = {"net", "http", "os"}  # network / process side effects
ANSI = re.compile(r"\033\[[0-9;]*m")

KEYWORDS = {
    "fn", "mut", "immut", "if", "else", "elseif", "for", "in", "while", "do", "return", "break",
    "continue", "compare", "matches", "otherwise", "done", "give", "to", "struct", "enum", "import",
    "static", "true", "false", "null", "int", "double", "bool", "char", "string", "void", "guess",
    "self", "unsafe", "defer", "step", "int32", "main", "default", "new",
}
TYPES = ["int", "string", "bool", "double", "char"]


# ----------------------------------------------------------------------------
# mutators: line (no newline) -> list of (kind, new_line). The compiler decides
# which candidates are real rejections.
# ----------------------------------------------------------------------------

def strip_strings(s: str) -> str:
    return re.sub(r'"(?:[^"\\]|\\.)*"', lambda m: '"' + "_" * (len(m.group(0)) - 2) + '"', s)


def is_comment(line: str) -> bool:
    return line.lstrip().startswith("//")


EXPR = r"""(?P<e>[A-Za-z_][\w.:]*(?:\([^()]*\))?|\d+(?:\.\d+)?|\([^()]*\)|"[^"]*"|'[^']*')"""
CAST_RE = re.compile(EXPR + r" to (?P<t>[A-Za-z_]\w*)\b")


def m_cast(line):
    out = []
    if is_comment(line):
        return []
    for i, m in enumerate(CAST_RE.finditer(re.sub(r'//.*$', lambda x: ' ' * len(x.group(0)), strip_strings(line)))):
        if i >= 2:
            break
        e, t = line[m.start("e"):m.end("e")], m.group("t")

        def sp(r, m=m):
            return line[: m.start()] + r + line[m.end():]

        out.append(("foreign_cast_c", sp(f"({t}){e}")))
        out.append(("foreign_cast_call", sp(f"{t}({e})" if not e.startswith("(") else f"{t}{e}")))
        if t == "string":
            out.append(("foreign_tostring", sp(f"str({e})")))
            out.append(("foreign_tostring", sp(f"{e}.toString()")))
        if t == "double":
            out.append(("foreign_cast_c", sp(f"float({e})")))
    return out


DECL_RE = re.compile(r"^(\s*)(mut|immut) (\w+): ([^=;]+?) = (.*)$")
DECL_NOINIT_RE = re.compile(r"^(\s*)(mut|immut) (\w+): ([^=;]+?);$")


def m_decl(line):
    out = []
    m = DECL_RE.match(line)
    if m:
        ind, kw, name, ty, rest = m.groups()
        out.append(("foreign_decl_c", f"{ind}{ty} {name} = {rest}"))
        out.append(("foreign_decl_let", f"{ind}let {name}: {ty} = {rest}"))
        out.append(("foreign_decl_var", f"{ind}var {name}: {ty} = {rest}"))
        if kw == "mut":
            out.append(("foreign_decl_let_mut", f"{ind}let mut {name}: {ty} = {rest}"))
        else:
            out.append(("foreign_decl_const", f"{ind}const {ty} {name} = {rest}"))
            out.append(("foreign_decl_const", f"{ind}const {name}: {ty} = {rest}"))
        out.append(("foreign_decl_walrus", f"{ind}{name} := {rest}"))
        out.append(("foreign_decl_auto", f"{ind}auto {name} = {rest}"))
        return out
    m = DECL_NOINIT_RE.match(line)
    if m:
        ind, kw, name, ty = m.groups()
        out.append(("foreign_decl_c", f"{ind}{ty} {name};"))
        out.append(("foreign_decl_let", f"{ind}let {name}: {ty};"))
    return out


TERN_RE = re.compile(r"(= |return |\()(.+?) give (.+?) otherwise (.+?)(;|\)|$)")


def m_ternary(line):
    m = TERN_RE.search(line)
    if not m:
        return []
    p, c, a, b, tail = m.groups()
    pre, post = line[: m.start()], line[m.end():]
    return [
        ("foreign_ternary", f"{pre}{p}{c} ? {a} : {b}{tail}{post}"),
        ("foreign_ternary", f"{pre}{p}{a} if {c} else {b}{tail}{post}"),
    ]


def m_dot(line):
    out = []
    s = strip_strings(line)
    m = re.search(r"\b([a-z_]\w*)::([a-z_]\w*)\(", s)
    if m:
        out.append(("foreign_module_dot", line[: m.start()] + f"{m.group(1)}.{m.group(2)}(" + line[m.end():]))
    m = re.search(r"\b([A-Z]\w*)::([A-Za-z_]\w*)", s)
    if m:
        name = m.group(2)
        out.append(("foreign_static_dot", line[: m.start()] + f"{m.group(1)}.{name}" + line[m.end():]))
        if name == "new":
            mm = re.search(r"\b([A-Z]\w*)::new\(", s)
            if mm:
                out.append(("foreign_new_keyword", line[: mm.start()] + f"new {mm.group(1)}(" + line[mm.end():]))
        if name == "default":
            mm = re.search(r"\b([A-Z]\w*)::default\(\)", s)
            if mm:
                out.append(("foreign_ctor_braces", line[: mm.start()] + f"{mm.group(1)}{{}}" + line[mm.end():]))
                out.append(("foreign_ctor_call", line[: mm.start()] + f"{mm.group(1)}()" + line[mm.end():]))
    return out


FN_RE = re.compile(r"^(\s*)fn(<[^>]*>)?( .*)$")


def m_fn(line):
    out = []
    m = FN_RE.match(line)
    if m:
        ind, gen, rest = m.groups()
        gen = gen or ""
        for kw in ("func", "def", "function", "fun"):
            out.append(("foreign_fn_keyword", f"{ind}{kw}{gen}{rest}"))
        r = re.search(r"\)\s*(static\s*)?:\s*([^\s{]+)\s*\{\s*$", line)
        if r:
            out.append(("foreign_return_arrow", line[: r.start()] + ") -> " + r.group(2) + " {"))
        pm = re.search(r"\(([^()]*)\)\s*(static\s*)?:", line)
        if pm and pm.group(1).strip():
            ps = [p.strip() for p in pm.group(1).split(",")]
            if all(re.fullmatch(r"\w+\s*:\s*[^:]+", p) for p in ps):
                c = ", ".join(re.sub(r"^(\w+)\s*:\s*(.+)$", r"\2 \1", p) for p in ps)
                out.append(("foreign_param_c", line[: pm.start(1)] + c + line[pm.end(1):]))
        mm = re.match(r"^(\s*)fn (\w+)\.(\w+)\(", line)
        if mm:
            out.append(("foreign_method_decl", f"{mm.group(1)}fn {mm.group(2)}::{mm.group(3)}(" + line[mm.end():]))
            out.append(("foreign_method_decl", f"{mm.group(1)}impl {mm.group(2)} {{ fn {mm.group(3)}(" + line[mm.end():]))
    return out


def m_for(line):
    out = []
    m = re.match(r"^(\s*)for (\w+) in (.+?)\.\.(.+?)\s*(step .+?)?\s*\{\s*$", line)
    if m:
        ind, v, a, b, _ = m.groups()
        out.append(("foreign_for_c", f"{ind}for (int {v} = {a}; {v} < {b}; {v}++) {{"))
        out.append(("foreign_for_range", f"{ind}for {v} in range({a}, {b}) {{"))
        out.append(("foreign_for_paren", f"{ind}for ({v} in {a}..{b}) {{"))
    m = re.match(r"^(\s*)for (\w+) in (\w+)\s*\{\s*$", line)
    if m:
        ind, v, arr = m.groups()
        out.append(("foreign_for_each", f"{ind}for (auto {v} : {arr}) {{"))
        out.append(("foreign_for_each", f"{ind}foreach ({v} in {arr}) {{"))
    return out


def m_switch(line):
    out = []
    m = re.match(r"^(\s*)compare (.+?)\s*\{\s*$", line)
    if m:
        out.append(("foreign_switch", f"{m.group(1)}switch ({m.group(2)}) {{"))
        out.append(("foreign_switch", f"{m.group(1)}match {m.group(2)} {{"))
    m = re.match(r"^(\s*)matches (.+):\s*$", line)
    if m:
        out.append(("foreign_case", f"{m.group(1)}case {m.group(2)}:"))
    ind = line[: len(line) - len(line.lstrip())]
    if line.strip() == "done":
        out.append(("foreign_break", f"{ind}break;"))
    if line.strip() == "otherwise:":
        out.append(("foreign_default", f"{ind}default:"))
    return out


def m_elif(line):
    if re.search(r"\belseif\b", line):
        return [("foreign_elif", re.sub(r"\belseif\b", w, line, count=1)) for w in ("elif", "elsif")]
    return []


def m_logic(line):
    s = strip_strings(line)
    out = []
    for op, word in (("&&", "and"), ("||", "or")):
        i = s.find(op)
        if i >= 0:
            out.append(("foreign_logic_word", line[:i] + word + line[i + 2:]))
    m = re.search(r"(?<![=!<>])!(?!=)(\w)", s)
    if m:
        out.append(("foreign_logic_word", line[: m.start()] + "not " + line[m.start() + 1:]))
    return out


def m_consts(line):
    s = strip_strings(line)
    out = []
    m = re.search(r"\bnull\b", s)
    if m:
        for w in ("nullptr", "None", "nil"):
            out.append(("foreign_null", line[: m.start()] + w + line[m.end():]))
    for lit, rep in (("true", "True"), ("false", "False")):
        m = re.search(rf"\b{lit}\b", s)
        if m:
            out.append(("foreign_bool_literal", line[: m.start()] + rep + line[m.end():]))
    m = re.match(r"^(\s*)immut ", line)
    if m:
        out.append(("foreign_const_keyword", f"{m.group(1)}const " + line[m.end():]))
    return out


def m_import(line):
    m = re.match(r"^import (\w+(?:::\w+)?);\s*$", line)
    if not m:
        return []
    n = m.group(1)
    return [
        ("foreign_import", f"#include <{n.split('::')[0]}>"),
        ("foreign_import", f"use {n};"),
        ("foreign_import", f"using {n};"),
        ("foreign_import", f"import {n.replace('::', '.')};"),
        ("foreign_import", f'import "{n.split("::")[0]}";'),
    ]


def m_print(line):
    out = []
    s = strip_strings(line)
    m = re.search(r"\b(?:io::)?printline\(", s)
    if m:
        for w in ("println(", "console.log(", "printf(", "puts("):
            out.append(("foreign_print", line[: m.start()] + w + line[m.end():]))
    return out


def m_method_call(line):
    s = strip_strings(line)
    out = []
    m = re.search(r"\b([a-z_]\w*)\.([a-z_]\w*)\(", s)
    if m:
        out.append(("foreign_arrow_call", line[: m.start()] + f"{m.group(1)}->{m.group(2)}(" + line[m.end():]))
    m = re.search(r"\b(\w+)\.length\(\)", s)
    if m:
        for r in (f"len({m.group(1)})", f"{m.group(1)}.length", f"{m.group(1)}.size()"):
            out.append(("foreign_length", line[: m.start()] + r + line[m.end():]))
    return out


def m_field(line):
    m = re.match(r"^(\s+)(\w+): (int|double|bool|char|string)(,?)\s*$", line)
    if m:
        ind, n, t, c = m.groups()
        return [("foreign_field_c", f"{ind}{t} {n}{c or ';'}")]
    return []


def m_array(line):
    m = re.search(r"= \[([^\[\]]*)\];", line)
    if m:
        return [("foreign_array_brace", line[: m.start()] + "= {" + m.group(1) + "};")]
    return []


def m_comment(line):
    m = re.match(r"^(\s*)//(.*)$", line)
    if m:
        return [("foreign_comment_hash", f"{m.group(1)}#{m.group(2)}")]
    return []


# --- classic four (+ dropped paren) ---

def m_semicolon(line):
    if line.rstrip().endswith(";") and not is_comment(line):
        i = line.rstrip().rfind(";")
        return [("delete_semicolon", line[:i] + line[i + 1:])]
    return []


def m_swap_type(line):
    out = []
    m = re.match(r"^(\s*(?:mut|immut) \w+: )(\w+)(.*)$", line)
    if m and m.group(2) in TYPES:
        for t in TYPES:
            if t != m.group(2):
                out.append(("swap_type", f"{m.group(1)}{t}{m.group(3)}"))
    m = re.match(r"^(.*\)\s*:\s*)(\w+)(\s*\{\s*)$", line)
    if m and m.group(2) in TYPES + ["void"] and line.lstrip().startswith("fn"):
        for t in ("string", "void", "bool", "int"):
            if t != m.group(2):
                out.append(("swap_return_type", f"{m.group(1)}{t}{m.group(3)}"))
    return out


def m_brace(line):
    # blanked rather than deleted so that exactly one line differs and line numbers stay put
    return [("drop_closing_brace", "")] if line.strip() == "}" else []


def m_typo(line):
    if is_comment(line):
        return []
    s = re.sub(r"//.*$", "", strip_strings(line))
    out = []
    for m in re.finditer(r"(?<![\w:.])([A-Za-z_]\w{2,})\b(?!\s*::)", s):
        name = m.group(1)
        if name in KEYWORDS or name in TYPES:
            continue
        typo = name[0] + name[2] + name[1] + name[3:]
        if typo != name:
            out.append(("typo_identifier", line[: m.start()] + typo + line[m.end():]))
    return out


def m_paren(line):
    s = strip_strings(line)
    if is_comment(line) or ")" not in s:
        return []
    i = s.rfind(")")
    return [("drop_paren", line[:i] + line[i + 1:])]


MUTATORS = [m_semicolon, m_swap_type, m_brace, m_typo, m_paren, m_cast, m_decl, m_ternary, m_dot, m_fn,
            m_for, m_switch, m_elif, m_logic, m_consts, m_import, m_print, m_method_call, m_field,
            m_array, m_comment]


def candidates(lines):
    """(1-based line, kind, new line without newline) for every candidate."""
    out = []
    for i, raw in enumerate(lines):
        line = raw.rstrip("\n")
        for mut in MUTATORS:
            if is_comment(line) and mut is not m_comment:
                continue
            for kind, new in mut(line):
                if new != line:
                    out.append((i + 1, kind, new))
    return out


# ----------------------------------------------------------------------------
# compiler plumbing
# ----------------------------------------------------------------------------
_tls = threading.local()
WORK: Path


def workdir() -> Path:
    if not hasattr(_tls, "d"):
        d = WORK / f"w{threading.get_ident()}"
        d.mkdir(parents=True, exist_ok=True)
        _tls.d = d
    return _tls.d


def clean_env():
    env = dict(os.environ)
    env.pop("TURF_HOME", None)
    return env


def run_compile(src: Path, exe: Path):
    try:
        p = subprocess.run([str(TURFC), str(src), "-O0", "-o", str(exe)], cwd=workdir(), capture_output=True,
                           text=True, timeout=TIMEOUT, stdin=subprocess.DEVNULL, env=clean_env(),
                           errors="replace")
    except subprocess.TimeoutExpired:
        return None, ""
    return p.returncode, ANSI.sub("", p.stderr + (("\n" + p.stdout) if p.stdout.strip() else ""))


# ----------------------------------------------------------------------------
# Programs: showcase/*.tr and the packages under examples/. Every one is mined
# (they are few), apart from the queue of test files that stops once the pool
# is full. A file of a package is compiled through a package graph rooted at
# the program package that reaches it, from a scratch copy of every package's
# src/ with the one file replaced.
# ----------------------------------------------------------------------------

def read_manifest(pkg_dir: Path):
    """-> (name, [dependency dirs]) from a turf.toml, or None."""
    t = pkg_dir / "turf.toml"
    if not t.exists():
        return None
    text = t.read_text()
    name = re.search(r'^\s*name\s*=\s*"([^"]+)"', text, re.M)
    deps = [(pkg_dir / p).resolve() for p in re.findall(r'path\s*=\s*"([^"]+)"', text)]
    return (name.group(1), deps) if name else None


def example_packages():
    """{rel .tr file: package} for every file of an examples/ program package
    (src/main.tr) and of the packages it depends on. A package is
    dict(dir=<rel program dir>, root=<name>, pkgs={name: rel src dir}, deps={name: [names]})."""
    out = {}
    for d in sorted((REPO / "examples").glob("*/")):
        m = read_manifest(d)
        if not m or not (d / "src" / "main.tr").exists():
            continue
        pkgs, deps, todo = {}, {}, [d]
        while todo:
            cur = todo.pop()
            cm = read_manifest(cur)
            if not cm or cm[0] in pkgs:
                continue
            pkgs[cm[0]] = str((cur / "src").relative_to(REPO))
            deps[cm[0]] = [read_manifest(x)[0] for x in cm[1] if read_manifest(x)]
            todo.extend(cm[1])
        pkg = dict(dir=str(d.relative_to(REPO)).rstrip("/"), root=m[0], pkgs=pkgs, deps=deps)
        for name, src in pkgs.items():
            for f in sorted((REPO / src).rglob("*.tr")):
                out.setdefault(str(f.relative_to(REPO)), pkg)
    return out


PACKAGES: dict = {}


def compile_package(rel: str, text: str, exe: Path):
    """Compile the program package that holds `rel`, with `rel` replaced by
    `text`. Paths in the output are the repository's."""
    pkg = PACKAGES[rel]
    base = workdir() / "pkg"
    shutil.rmtree(base, ignore_errors=True)
    graph = {"format": 1, "root": pkg["root"], "packages": {}}
    for name, src in pkg["pkgs"].items():
        shutil.copytree(REPO / src, base / name)
        graph["packages"][name] = {"src": str(base / name), "deps": pkg["deps"][name],
                                   "allow_unsafe": name == pkg["root"]}
    owner = next(n for n, s in pkg["pkgs"].items() if rel.startswith(s + "/"))
    (base / owner / Path(rel).relative_to(pkg["pkgs"][owner])).write_text(text)
    (base / "graph.json").write_text(json.dumps(graph))
    root_main = base / pkg["root"] / "main.tr"
    try:
        p = subprocess.run([str(TURFC), "--package-graph", str(base / "graph.json"), str(root_main), "-O0",
                            "-o", str(exe)], cwd=workdir(), capture_output=True, text=True, timeout=TIMEOUT,
                           stdin=subprocess.DEVNULL, env=clean_env(), errors="replace")
    except subprocess.TimeoutExpired:
        return None, ""
    out = ANSI.sub("", p.stderr + (("\n" + p.stdout) if p.stdout.strip() else ""))
    for name, src in pkg["pkgs"].items():  # turfc names a file relative to its cwd when it can
        out = out.replace(str(base / name), src)
        out = re.sub(r"(?<![\w/.-])" + re.escape(f"pkg/{name}") + r"(?=/)", src, out)
    return p.returncode, out


def compile_source(rel: str, text: str, exe: Path, name: str = "m.tr"):
    if exe.exists():
        exe.unlink()
    if rel in PACKAGES:
        return compile_package(rel, text, exe)
    src = workdir() / name
    src.write_text(text)
    code, out = run_compile(src, exe)
    return code, re.sub(r"(?<![\w/.-])" + re.escape(name) + r"\b", rel, out.replace(str(src), rel))


def run_twice(exe: Path):
    """-> stdout of a program that exits 0 the same way twice, or a reason string."""
    outs = []
    for _ in range(2):
        try:
            r = subprocess.run([str(exe)], cwd=workdir(), capture_output=True, text=True, timeout=TIMEOUT,
                               stdin=subprocess.DEVNULL, env=clean_env(), errors="replace")
        except subprocess.TimeoutExpired:
            return "original_timeout"
        if r.returncode != 0:
            return "original_nonzero_exit"
        outs.append(r.stdout)
    if outs[0] != outs[1]:
        return "original_nondeterministic"
    return {"expected_stdout": outs[0], "expected_exit": 0}


def program_original(rel: str):
    """Like check_original, for a program: a program that compiles but cannot
    be run here (network, a nonzero exit, output that varies) still gives
    cases, with oracle `recompile`. -> dict or a skip-reason string."""
    text = (REPO / rel).read_text(errors="replace")
    imports = {m.group(1).split("::")[0].strip() for m in re.finditer(r"^\s*import\s+([^;\"]+);", text, re.M)}
    exe = workdir() / "orig.bin"
    code, _ = compile_source(rel, text, exe)
    if code != 0:
        return "original_does_not_compile"
    run = "side_effect_import" if imports & SKIP_IMPORTS else run_twice(exe)
    if isinstance(run, dict):
        return dict(run, oracle="asserts")
    return {"expected_stdout": "", "expected_exit": None, "oracle": "recompile"}


def check_original(rel: str):
    """-> dict(expected_stdout, expected_exit) or a skip-reason string."""
    text = (REPO / rel).read_text(errors="replace")
    if re.search(r"RUNTIME_ABORT_EXPECTED|EXPECT-RUNTIME|EXPECT-EXIT", text):
        return "expects_runtime_failure"
    for m in re.finditer(r"^\s*import\s+([^;]+);", text, re.M):
        spec = m.group(1).strip()
        if spec.startswith('"'):
            return "local_import"
        head = spec.split("::")[0].strip()
        if not list(STD.rglob(head + ".tr")):
            return "local_import"
        if head in SKIP_IMPORTS:
            return "side_effect_import"
    d = workdir()
    exe = d / "orig.bin"
    if exe.exists():
        exe.unlink()
    code, _ = run_compile(REPO / rel, exe)
    if code != 0:
        return "original_does_not_compile"
    outs = []
    for _ in range(2):
        try:
            r = subprocess.run([str(exe)], cwd=d, capture_output=True, text=True, timeout=TIMEOUT,
                               stdin=subprocess.DEVNULL, env=clean_env(), errors="replace")
        except subprocess.TimeoutExpired:
            return "original_timeout"
        if r.returncode != 0:
            return "original_nonzero_exit"
        outs.append(r.stdout)
    if outs[0] != outs[1]:
        return "original_nondeterministic"
    return {"expected_stdout": outs[0], "expected_exit": 0}


HEADER = re.compile(r"──\s*error at line (\d+)(?:, col \d+)?\s*(?:\[(E\d+)[^\]]*\])?")


def evaluate(rel, src_text, lines, line_no, new):
    """Compile a mutant. -> field dict, or a reject-reason string."""
    ml = list(lines)
    nl = "\n" if ml[line_no - 1].endswith("\n") else ""
    ml[line_no - 1] = new + nl
    mutated = "".join(ml)
    if mutated == src_text:
        return "no_change"
    code, out = compile_source(rel, mutated, workdir() / "m.bin")
    if code is None:
        return "timeout"
    if code == 0:
        return "accepted_by_compiler"
    if code != 1:
        return "crash_or_usage_exit"
    if re.search(r"Internal compiler error|LLVM ERROR|Assertion|Segmentation", out):
        return "internal_error"
    headers = list(HEADER.finditer(out))
    if not headers:
        return "no_error_header"
    nerr = re.search(r"\((\d+) errors?", out)
    nerr = int(nerr.group(1)) if nerr else len(headers)
    first = headers[0]
    diag_line = int(first.group(1))
    if nerr != 1 and abs(diag_line - line_no) > 3:
        return "first_error_far_from_mutation"
    if rel in PACKAGES and not out.lstrip().startswith(rel):
        return "first_error_in_another_file"
    diag = out
    after = out[first.end():].splitlines()
    msg = next((l.strip() for l in after if l.strip() and not l.strip().startswith("──")), "")
    return {
        "line": line_no,
        "original_line": lines[line_no - 1].rstrip("\n"),
        "mutated_line": new,
        "mutated_source": mutated,
        "diagnostic": diag,
        "error_code": first.group(2) or "",
        "error_message": msg,
        "diag_line": diag_line,
    }


def split_of(rel: str) -> str:
    return "test" if int(hashlib.sha1(rel.encode()).hexdigest(), 16) % 100 < 25 else "train"


def mine_file(rel, orig, seed, max_try, want):
    """Try kind-stratified candidates for one file. -> (accepted list, reject Counter)."""
    text = (REPO / rel).read_text(errors="replace")
    lines = text.splitlines(keepends=True)
    rng = random.Random(f"{seed}:{rel}")
    bykind = collections.defaultdict(list)
    for c in candidates(lines):
        bykind[c[1]].append(c)
    kinds = sorted(bykind)
    for k in kinds:
        rng.shuffle(bykind[k])
    rng.shuffle(kinds)
    order = []
    while any(bykind[k] for k in kinds):
        for k in kinds:
            if bykind[k]:
                order.append(bykind[k].pop())
    rej = collections.Counter()
    acc, seen, tried = [], set(), 0
    for line_no, kind, new in order:
        if tried >= max_try or len(acc) >= want:
            break
        tried += 1
        r = evaluate(rel, text, lines, line_no, new)
        if isinstance(r, str):
            rej[(kind, r)] += 1
            continue
        if r["mutated_source"] in seen:
            rej[(kind, "duplicate")] += 1
            continue
        seen.add(r["mutated_source"])
        r.update(kind=kind, source_test=rel, expected_stdout=orig["expected_stdout"],
                 expected_exit=orig["expected_exit"], oracle=orig.get("oracle", "asserts"),
                 package=PACKAGES[rel]["dir"] if rel in PACKAGES else None)
        acc.append(r)
    return acc, rej


def select(pool, target, per_file_cap, kind_cap_frac, rng):
    """Greedy balanced pick: least-used kind first, then rarest error code, then least-used file."""
    pool = sorted(pool, key=lambda e: (e["source_test"], e["line"], e["kind"], e["mutated_line"]))
    rng.shuffle(pool)
    kinds, codes, files = (collections.Counter() for _ in range(3))
    chosen = []
    cap = max(1, int(target * kind_cap_frac))
    while pool and len(chosen) < target:
        best = None
        for e in pool:
            if files[e["source_test"]] >= per_file_cap or kinds[e["kind"]] >= cap:
                continue
            score = (kinds[e["kind"]], codes[e["error_code"]], files[e["source_test"]])
            if best is None or score < best[0]:
                best = (score, e)
        if best is None:
            break
        e = best[1]
        pool.remove(e)
        kinds[e["kind"]] += 1
        codes[e["error_code"]] += 1
        files[e["source_test"]] += 1
        chosen.append(e)
    return chosen


FIELDS = ["id", "split", "source_test", "package", "kind", "oracle", "line", "original_line", "mutated_line", "mutated_source",
          "diagnostic", "error_code", "error_message", "diag_line", "expected_stdout", "expected_exit"]
SPLIT_FILE = {"train": "train", "test": "heldout"}


def main():
    global WORK
    ap = argparse.ArgumentParser()
    ap.add_argument("--jobs", type=int, default=2)
    ap.add_argument("--seed", type=int, default=20260)
    ap.add_argument("--train-target", type=int, default=320)
    ap.add_argument("--test-target", type=int, default=72)
    ap.add_argument("--max-try", type=int, default=30, help="candidates compiled per source file")
    ap.add_argument("--out-dir", default=None, help="write outputs here instead of corpus/ (for reproducibility checks)")
    ap.add_argument("--per-file-pool", type=int, default=4, help="accepted mutants kept per file before selection")
    ap.add_argument("--multi-per-split", type=int, default=24, help="two-error cases per split")
    ap.add_argument("--per-program", type=int, default=4,
                    help="cases per showcase/ program and per file of an examples/ package")
    args = ap.parse_args()
    global OUT
    if args.out_dir:
        OUT = Path(args.out_dir).resolve()
    if not TURFC.exists():
        sys.exit(f"missing {TURFC}")
    own_work = not os.environ.get("CORPUS_WORK")
    WORK = Path(os.environ.get("CORPUS_WORK") or tempfile.mkdtemp(prefix="rag_corpus_"))
    WORK.mkdir(parents=True, exist_ok=True)
    seed = args.seed

    global PACKAGES
    PACKAGES = example_packages()
    skipped = collections.Counter()
    elig = []
    for p in sorted((REPO / "tests").rglob("*.tr")):
        f = str(p.relative_to(REPO))
        if p.name.startswith("fail_"):
            skipped["fail_* file"] += 1
        elif f.startswith(SKIP_DIRS):
            skipped["excluded directory"] += 1
        else:
            elig.append(f)
    order = {s: sorted([f for f in elig if split_of(f) == s],
                       key=lambda p: hashlib.sha1((str(seed) + p).encode()).hexdigest())
             for s in ("train", "test")}

    pools = {"train": [], "test": []}
    rejects = collections.Counter()
    mined = {"train": 0, "test": 0}
    pool_target = {"train": int(args.train_target * 5), "test": int(args.test_target * 8)}

    with cf.ThreadPoolExecutor(args.jobs) as ex:
        for split in ("test", "train"):
            fl = order[split]
            pos, batch = 0, args.jobs * 6
            while pos < len(fl) and len(pools[split]) < pool_target[split]:
                chunk = fl[pos:pos + batch]
                pos += batch
                good = []
                for f, o in zip(chunk, ex.map(check_original, chunk)):
                    if isinstance(o, str):
                        skipped[o] += 1
                    else:
                        good.append((f, o))
                for acc, rej in ex.map(lambda fo: mine_file(fo[0], fo[1], seed, args.max_try, args.per_file_pool), good):
                    mined[split] += 1
                    pools[split].extend(acc)
                    rejects.update(rej)
                print(f"[{split}] files {min(pos, len(fl))}/{len(fl)} pool {len(pools[split])}", file=sys.stderr)

    rng = random.Random(seed)
    test = select(pools["test"], args.test_target, 3, 0.30, rng)
    train = select(pools["train"], args.train_target, 4, 0.30, rng)
    for e in train + test:
        e.setdefault("oracle", "asserts")
    with cf.ThreadPoolExecutor(args.jobs) as ex:
        for split, ents in (("train", train), ("test", test)):
            multi = multi_error_cases(pools[split], args.multi_per_split, rng, ex)
            ents.extend(multi)
        progs = [str(p.relative_to(REPO)) for p in sorted((REPO / "showcase").glob("*.tr"))] + sorted(PACKAGES)
        programs = collections.Counter()
        for f, o in zip(progs, ex.map(program_original, progs)):
            if isinstance(o, str):
                skipped[o] += 1
                continue
            acc, rej = mine_file(f, o, seed, args.max_try, args.per_program)
            rejects.update(rej)
            programs[o["oracle"]] += 1
            (test if split_of(f) == "test" else train).extend(acc)
        mined["programs"] = programs
        covered = {e["error_code"] for e in train + test}
        for e in fail_test_cases(covered, ex):
            (test if split_of(e["source_test"]) == "test" else train).append(e)
    for e in hard_negative_cases():
        (test if split_of(e["source_test"]) == "test" else train).append(e)
    OUT.mkdir(exist_ok=True)
    for name, ents in (("train", train), ("test", test)):
        ents.sort(key=lambda e: (e["source_test"], e["line"], e["kind"]))
        fname = SPLIT_FILE[name]
        with open(OUT / f"{fname}.jsonl", "w") as fh:
            for i, e in enumerate(ents):
                e["split"] = fname
                h = hashlib.sha1((e["source_test"] + str(e["line"]) + e["kind"] + e["mutated_line"]).encode()).hexdigest()[:8]
                e["id"] = f"{fname}-{i:04d}-{h}"
                fh.write(json.dumps({k: e.get(k) for k in FIELDS}, ensure_ascii=False) + "\n")
    write_stats(train, test, skipped, rejects, pools, mined, args)
    if own_work:
        shutil.rmtree(WORK, ignore_errors=True)
    print(f"train={len(train)} test={len(test)}")


# ----------------------------------------------------------------------------
# SL-3 additions: two-error cases, one case per otherwise-missing error code,
# hard negatives.
# ----------------------------------------------------------------------------

def compile_text(text: str):
    d = workdir()
    src = d / "x.tr"
    src.write_text(text)
    exe = d / "x.bin"
    if exe.exists():
        exe.unlink()
    return run_compile(src, exe)


def multi_error_cases(pool, want, rng, ex):
    """Two mutants of one file, at least 5 lines apart, reported as >= 2 errors."""
    byfile = collections.defaultdict(list)
    for e in pool:
        byfile[e["source_test"]].append(e)
    pairs = []
    for f in sorted(byfile):
        es = sorted(byfile[f], key=lambda e: e["line"])
        for i in range(len(es)):
            for j in range(i + 1, len(es)):
                if es[j]["line"] - es[i]["line"] >= 5:
                    pairs.append((es[i], es[j]))
                    break
            else:
                continue
            break
    rng.shuffle(pairs)

    def make(pair):
        a, b = pair
        lines = (REPO / a["source_test"]).read_text(errors="replace").splitlines(keepends=True)
        for e in (a, b):
            nl = "\n" if lines[e["line"] - 1].endswith("\n") else ""
            lines[e["line"] - 1] = e["mutated_line"] + nl
        text = "".join(lines)
        code, out = compile_text(text)
        if code != 1 or re.search(r"Internal compiler error|LLVM ERROR", out):
            return None
        n = re.search(r"\((\d+) errors?", out)
        if not n or int(n.group(1)) < 2:
            return None
        first = HEADER.search(out)
        after = out[first.end():].splitlines()
        return dict(a, kind=f"multi:{a['kind']}+{b['kind']}", line=a["line"],
                    mutated_line=a["mutated_line"] + " | " + b["mutated_line"],
                    original_line=a["original_line"] + " | " + b["original_line"],
                    mutated_source=text, diagnostic=out.replace(str(workdir() / "x.tr"), a["source_test"]),
                    error_code=first.group(2) or "", diag_line=int(first.group(1)),
                    error_message=next((l.strip() for l in after if l.strip()), ""), oracle="asserts")

    got = []
    for r in ex.map(make, pairs[: want * 3]):
        if r and len(got) < want:
            got.append(r)
    return got


def fail_test_cases(covered, ex):
    """For each code a single-error fail_*.tr reports and no mutant reached,
    the shortest such test. Its oracle is `recompile`: no original program."""
    files = [p for p in sorted((REPO / "tests").rglob("fail_*.tr"))
             if not str(p.relative_to(REPO)).startswith(SKIP_DIRS)]

    def probe(p):
        text = p.read_text(errors="replace")
        if re.search(r"RUNTIME_ABORT_EXPECTED", text) or re.search(r"^\s*import\s+\"", text, re.M):
            return None
        code, out = compile_text(text)
        if code != 1:
            return None
        hs = list(HEADER.finditer(out))
        n = re.search(r"\((\d+) errors?", out)
        if len(hs) != 1 or (n and int(n.group(1)) != 1) or not hs[0].group(2):
            return None
        rel = str(p.relative_to(REPO))
        after = out[hs[0].end():].splitlines()
        return dict(source_test=rel, kind="fail_test", oracle="recompile", line=int(hs[0].group(1)),
                    original_line="", mutated_line="", mutated_source=text,
                    diagnostic=out.replace(str(workdir() / "x.tr"), rel), error_code=hs[0].group(2),
                    error_message=next((l.strip() for l in after if l.strip()), ""),
                    diag_line=int(hs[0].group(1)), expected_stdout="", expected_exit=None)

    best = {}
    for r in ex.map(probe, files):
        if not r or r["error_code"] in covered:
            continue
        cur = best.get(r["error_code"])
        if cur is None or len(r["mutated_source"]) < len(cur["mutated_source"]):
            best[r["error_code"]] = r
    return [best[c] for c in sorted(best)]


def hard_negative_cases():
    """hard_negatives/*.tr: a header `// HARD-NEGATIVE: <why>` and a program
    the compiler rejects; the right outcome is no suggestion."""
    out = []
    for p in sorted(HARD.glob("*.tr")):
        text = p.read_text()
        why = re.search(r"^// HARD-NEGATIVE: (.+)$", text, re.M)
        code, diag = compile_text(text)
        h = HEADER.search(diag)
        if not why or code != 1 or not h:
            sys.exit(f"{p}: a hard negative needs a `// HARD-NEGATIVE:` line and a compile error")
        rel = str(p.relative_to(REPO))
        after = diag[h.end():].splitlines()
        out.append(dict(source_test=rel, kind="hard_negative", oracle="abstain", line=int(h.group(1)),
                        original_line="", mutated_line=why.group(1), mutated_source=text,
                        diagnostic=diag.replace(str(workdir() / "x.tr"), rel), error_code=h.group(2) or "",
                        error_message=next((l.strip() for l in after if l.strip()), ""),
                        diag_line=int(h.group(1)), expected_stdout="", expected_exit=None))
    return out


def table(counter, head):
    rows = [f"| {head} | count |", "|---|---|"]
    for k, v in sorted(counter.items(), key=lambda kv: (-kv[1], str(kv[0]))):
        rows.append(f"| {k or '(none)'} | {v} |")
    return "\n".join(rows)


def uncovered_codes(cases):
    """Error codes a test expects (EXPECT-DIAG, @test(compile_error, ...)) that no case has.
    The rest are warnings, need another module, have no source line, or are made up by a test."""
    want = set()
    for p in (REPO / "tests").rglob("*.tr"):
        if str(p.relative_to(REPO)).startswith(SKIP_DIRS):
            continue
        t = p.read_text(errors="replace")
        want |= set(re.findall(r"EXPECT-DIAG:?\s*(E\d{3})", t)) | set(re.findall(r"@test\(compile_error,\s*(E\d{3})", t))
    miss = sorted(want - {c["error_code"] for c in cases})
    return ["## Codes the suite expects that no case has", "",
            "A warning, an error that needs a second module or package, an error with no source line, "
            "or a code a test makes up. Count: " + str(len(miss)) + ".", "", ", ".join(miss) or "none", ""]


def write_stats(train, test, skipped, rejects, pools, mined, args):
    L = ["# Corpus statistics", "",
         f"Generated by `build_corpus.py` (seed {args.seed}). Split is by sha1 of the source path (25% heldout).", "",
         "## Entries by split", "", "| split | entries | distinct source files | distinct error codes |", "|---|---|---|---|"]
    for name, e in (("train", train), ("heldout", test)):
        L.append(f"| {name} | {len(e)} | {len({x['source_test'] for x in e})} | {len({x['error_code'] for x in e})} |")
    L.append("")
    allk = collections.defaultdict(lambda: [0, 0])
    for i, e in enumerate((train, test)):
        for x in e:
            allk[x["kind"]][i] += 1
    L += ["## By kind", "", "| kind | train | heldout | heldout % |", "|---|---|---|---|"]
    for k, (a, b) in sorted(allk.items(), key=lambda kv: -(kv[1][0] + kv[1][1])):
        L.append(f"| {k} | {a} | {b} | {100 * b / max(1, len(test)):.1f}% |")
    fam = collections.Counter("foreign" if x["kind"].startswith("foreign") else "classic" for x in test)
    famt = collections.Counter("foreign" if x["kind"].startswith("foreign") else "classic" for x in train)
    L += ["", f"Families, heldout: {dict(fam)}; train: {dict(famt)}", ""]
    codes = collections.defaultdict(lambda: [0, 0])
    for i, e in enumerate((train, test)):
        for x in e:
            codes[x["error_code"]][i] += 1
    L += ["## By error code", "", "| code | train | heldout |", "|---|---|---|"]
    for k, (a, b) in sorted(codes.items(), key=lambda kv: -(kv[1][0] + kv[1][1])):
        L.append(f"| {k or '(none)'} | {a} | {b} |")
    L += ["", "## Source files skipped", "", table(skipped, "reason"), "",
          f"Test files mined: train {mined['train']}, heldout {mined['test']} "
          "(mining stops once the candidate pool is large enough; remaining files are not tried, so they are not counted above).", "",
          "Programs (every `showcase/` file and every file of an `examples/` package, all mined) by oracle: "
          + (", ".join(f"{k} {v}" for k, v in sorted(mined["programs"].items())) or "none") + ".", "",
          f"Candidate pool before balanced selection: train {len(pools['train'])}, heldout {len(pools['test'])}.", ""]
    L += uncovered_codes(train + test)
    byreason = collections.Counter()
    bykind = collections.defaultdict(collections.Counter)
    for (k, r), n in rejects.items():
        byreason[r] += n
        bykind[k][r] += n
    L += ["## Rejected mutation candidates (by reason)", "",
          f"Total compiled candidates rejected: {sum(byreason.values())}", "", table(byreason, "reason"), "",
          "## Rejection by kind", "",
          "| kind | rejected total | accepted_by_compiler | first_error_far | other |", "|---|---|---|---|---|"]
    for k, c in sorted(bykind.items(), key=lambda kv: -sum(kv[1].values())):
        t = sum(c.values())
        a, f = c.get("accepted_by_compiler", 0), c.get("first_error_far_from_mutation", 0)
        L.append(f"| {k} | {t} | {a} | {f} | {t - a - f} |")
    (OUT / "STATS.md").write_text("\n".join(L) + "\n")


if __name__ == "__main__":
    main()
