#!/usr/bin/env python3
"""elision_diff.py -- NM-2 (issue #348): differential check of check elision.

ValueRangePass::markElidableChecks() removes an overflow or bounds check only
where its value ranges prove the check can never fire (F-OPT-2). A wrong range
removes a check that was needed, and nothing else notices: the program wraps
or reads past an array instead of panicking. This script builds the same
program twice, with elision and with TURF_NO_ELISION=1 (every check kept),
runs both, and requires the same exit status and the same output.

Programs come from two places:

  * the corpus: every plain program under tests/ (not compat/, custom/,
    99_known_bugs/, `fail_*`, `*_module.tr`, `@test` files), showcase/ and
    examples/;
  * generated programs (--seconds N): random straight-line code, `if` guards,
    `while` and range-`for` loops, `%`, `&`, `>>`, `<<`, compound assignment
    and array indexing over values near 0 and near the `int` limits, the
    shapes the ranges reason about. Each program prints every variable it
    computes, so a wrapped value or a missing panic changes the output.

A difference is a finding: the program is written to --out with both
results, and the exit status is 1.

Usage: scripts/dev/fuzz/elision_diff.py [--corpus] [--seconds N] [--seed S]
           [--jobs J] [--compiler PATH] [--out DIR] [--timeout SEC]
           [--opt -O0|-O2|...]
Exit:  0 = no difference; 1 = at least one; 2 = the harness itself failed.
"""
from __future__ import annotations

import argparse
import os
import random
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
INT_MAX = 2**63 - 1

# ── the corpus ──────────────────────────────────────────────────────────────


def corpus_programs() -> list[Path]:
    out = []
    for root in ("tests", "showcase", "examples"):
        for p in sorted((REPO / root).rglob("*.tr")):
            rel = p.relative_to(REPO).as_posix()
            if any(part in rel for part in ("tests/compat/", "tests/custom/", "tests/99_known_bugs/")):
                continue
            if p.name.startswith("fail_") or p.name.endswith("_module.tr"):
                continue
            text = p.read_text(errors="replace")
            if any(line.startswith("@test") for line in text.splitlines()):
                continue
            if (p.parent / "turf.toml").exists() or "fn main" not in text:
                continue
            out.append(p)
    return out


# ── generated programs ──────────────────────────────────────────────────────

INTERESTING = [0, 1, -1, 2, 3, 7, 10, 15, 16, 63, 64, 255, 1000,
               2**31 - 1, -2**31, 2**53, 2**62, INT_MAX - 3, INT_MAX - 1, -INT_MAX + 2]


class Gen:
    def __init__(self, rng: random.Random):
        self.r = rng
        self.vars: list[str] = []
        self.loop_vars: list[str] = []
        self.guards = 0
        self.names = 0  # every declared name is new: no shadowing, no duplicates
        self.size = self.r.randint(1, 16)

    def lit(self) -> str:
        v = self.r.choice(INTERESTING) if self.r.random() < 0.3 else self.r.randint(-20, 20)
        return f"(0 - {-v})" if v < 0 else str(v)

    def atom(self) -> str:
        pool = self.vars + self.loop_vars + ["p", "q"]
        return self.r.choice(pool) if self.r.random() < 0.7 else self.lit()

    def expr(self, depth: int = 0) -> str:
        if depth > 2 or self.r.random() < 0.35:
            return self.atom()
        k = self.r.randrange(9)
        a = self.expr(depth + 1)
        if k == 0:
            return f"({a} % {self.r.choice([2, 3, 7, 10, 16, 1000, -5])})"
        if k == 1:
            return f"({a} & {self.r.choice([1, 7, 15, 255, 1023])})"
        if k == 2:
            return f"({a} >> {self.r.randint(0, 63)})"
        if k == 3:
            return f"({a} << {self.r.randint(0, 8)})"
        if k == 4:
            return f"(0 - {a})"
        op = self.r.choice(["+", "-", "*"])
        return f"({a} {op} {self.expr(depth + 1)})"

    def cond(self) -> str:
        op = self.r.choice(["<", "<=", ">", ">=", "==", "!="])
        c = f"{self.atom()} {op} {self.expr(1)}"
        roll = self.r.random()
        if roll < 0.15:
            return f"!({c})"
        if roll < 0.3:
            return f"{c} && {self.atom()} {self.r.choice(['<', '>='])} {self.lit()}"
        if roll < 0.4:
            return f"{c} || {self.atom()} {self.r.choice(['>', '<='])} {self.lit()}"
        return c

    def index(self) -> str:
        roll = self.r.random()
        if self.loop_vars and roll < 0.4:
            return self.r.choice(self.loop_vars)
        if roll < 0.7:
            return f"({self.r.choice(self.vars + ['p', 'q'])} % {self.size})"
        return self.r.choice(self.vars + ['p', 'q'])

    def block(self, depth: int, ind: str) -> list[str]:
        # A block's own declarations go out of scope at its `}`.
        saved = len(self.vars)
        out = self.stmts(depth + 1, self.r.randint(1, 3), ind)
        del self.vars[saved:]
        return out

    def stmts(self, depth: int, n: int, ind: str) -> list[str]:
        out = []
        for _ in range(n):
            k = self.r.randrange(10)
            if not self.vars or k == 0:
                v = f"v{self.names}"
                self.names += 1
                out.append(f"{ind}mut {v}: int = {self.expr()};")
                self.vars.append(v)
            elif k == 1:
                out.append(f"{ind}{self.r.choice(self.vars)} = {self.expr()};")
            elif k == 2:
                out.append(f"{ind}{self.r.choice(self.vars)} {self.r.choice(['+=', '-=', '*='])} {self.expr(1)};")
            elif k == 3:
                out.append(f"{ind}{self.r.choice(self.vars)}{self.r.choice(['++', '--'])};")
            elif k == 4:
                out.append(f"{ind}a[{self.index()}] = {self.expr(1)};")
            elif k == 5:
                out.append(f"{ind}{self.r.choice(self.vars)} = a[{self.index()}];")
            elif k == 6 and depth < 3:
                out.append(f"{ind}if {self.cond()} {{")
                out += self.block(depth, ind + "    ")
                if self.r.random() < 0.4:
                    out.append(f"{ind}}} else {{")
                    out += self.block(depth, ind + "    ")
                out.append(f"{ind}}}")
            elif k == 7 and depth < 3:
                g = f"g{self.guards}"
                self.guards += 1
                out.append(f"{ind}mut {g}: int = 0;")
                out.append(f"{ind}while {self.cond()} {{")
                out.append(f"{ind}    {g} += 1;")
                out.append(f"{ind}    if {g} > 40 {{")
                out.append(f"{ind}        break;")
                out.append(f"{ind}    }}")
                out += self.block(depth, ind + "    ")
                out.append(f"{ind}}}")
            elif k == 8 and depth < 3:
                i = f"i{self.names}"
                fg = f"f{self.names}g"
                self.names += 1
                lo, hi = self.r.choice([("0", str(self.size)), ("0", "p % 20"), (self.lit(), self.lit()),
                                        (str(self.size), "0"), ("q % 10", "q % 30")])
                step = self.r.choice(["", "", f" step {i}++", f" step {i} += 2", f" step {i}--", f" step {i} -= 3"])
                out.append(f"{ind}mut {fg}: int = 0;")
                out.append(f"{ind}for {i} in {lo}..{hi}{step} {{")
                out.append(f"{ind}    {fg} += 1;")
                out.append(f"{ind}    if {fg} > 40 {{")
                out.append(f"{ind}        break;")
                out.append(f"{ind}    }}")
                self.loop_vars.append(i)
                out += self.block(depth, ind + "    ")
                self.loop_vars.pop()
                out.append(f"{ind}}}")
            else:
                out.append(f"{ind}io::printline({self.expr()});")
        return out

    def program(self) -> str:
        body = self.stmts(0, self.r.randint(4, 12), "    ")
        prints = [f"    io::printline({v});" for v in self.vars]
        p, q = self.r.choice(INTERESTING + [5, 12]), self.r.choice(INTERESTING + [5, 12])
        lit = lambda v: f"(0 - {-v})" if v < 0 else str(v)
        return "\n".join(
            ["import io;", "", "fn body(p: int, q: int): int {", f"    mut a: int[{self.size}];"]
            + body + prints + ["    return 0;", "}", "",
                               "fn main(): int {", f"    return body({lit(p)}, {lit(q)});", "}", ""])


# ── build and run ───────────────────────────────────────────────────────────


def build_and_run(compiler: str, src: Path, exe: Path, elide: bool, opt: str, timeout: int):
    env = dict(os.environ)
    env.pop("TURF_HOME", None)
    if not elide:
        env["TURF_NO_ELISION"] = "1"
    cmd = [compiler, src.name, "-o", str(exe)] + ([opt] if opt else [])
    try:
        c = subprocess.run(cmd, cwd=src.parent, env=env, capture_output=True, timeout=timeout * 4)
    except subprocess.TimeoutExpired:
        return ("compile-timeout", b"")
    if c.returncode != 0:
        return (f"compile-{c.returncode}", b"")
    try:
        runs = []
        for _ in range(2):
            r = subprocess.run([str(exe)], cwd=src.parent, capture_output=True, timeout=timeout,
                               stdin=subprocess.DEVNULL)
            runs.append((r.returncode, r.stdout + b"\n--stderr--\n" + r.stderr))
        # A program whose two runs differ (random numbers, the clock) says
        # nothing about elision.
        return runs[0] if runs[0] == runs[1] else ("nondeterministic", b"")
    except subprocess.TimeoutExpired:
        return ("run-timeout", b"")
    finally:
        exe.unlink(missing_ok=True)


def compare(compiler, src: Path, scratch: Path, opt, timeout):
    stem = f"{src.stem}_{os.getpid()}_{random.getrandbits(32):08x}"
    on = build_and_run(compiler, src, scratch / (stem + "_on"), True, opt, timeout)
    off = build_and_run(compiler, src, scratch / (stem + "_off"), False, opt, timeout)
    # A timeout or a nondeterministic program says nothing about elision.
    if {"run-timeout", "compile-timeout", "nondeterministic"} & {on[0], off[0]}:
        return None
    return None if on == off else (on, off)


def report(out: Path, name: str, text: str, on, off) -> None:
    out.mkdir(parents=True, exist_ok=True)
    (out / f"{name}.tr").write_text(text)
    (out / f"{name}.txt").write_bytes(
        b"elided: exit %s\n%s\n\nall checks: exit %s\n%s\n" % (str(on[0]).encode(), on[1], str(off[0]).encode(), off[1]))
    print(f"DIFFERENCE {name}: elided exit {on[0]}, all checks exit {off[0]} -> {out / name}.tr")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--corpus", action="store_true", help="compare every program of the corpus")
    ap.add_argument("--seconds", type=int, default=0, help="generate programs for this long")
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--jobs", type=int, default=2)
    ap.add_argument("--compiler", default=str(REPO / "build" / "turfc"))
    ap.add_argument("--out", default=str(REPO / "fuzz-findings" / "elision"))
    ap.add_argument("--timeout", type=int, default=10)
    ap.add_argument("--opt", default="")
    a = ap.parse_args()
    if not a.corpus and not a.seconds:
        ap.error("give --corpus, --seconds N, or both")
    if not Path(a.compiler).exists():
        print(f"elision_diff: no compiler at {a.compiler}", file=sys.stderr)
        return 2
    out = Path(a.out)
    findings = 0

    if a.corpus:
        progs = corpus_programs()
        with tempfile.TemporaryDirectory(prefix="elision_diff_") as tmp, ThreadPoolExecutor(a.jobs) as ex:
            results = list(ex.map(lambda p: (p, compare(a.compiler, p, Path(tmp), a.opt, a.timeout)), progs))
        for p, diff in results:
            if diff:
                findings += 1
                report(out, p.stem, p.read_text(), *diff)
        print(f"elision_diff: corpus: {len(progs)} programs, {findings} difference(s)")

    if a.seconds:
        seed = a.seed if a.seed is not None else random.randrange(2**32)
        rng = random.Random(seed)
        deadline = time.time() + a.seconds
        n = gen_findings = 0
        with tempfile.TemporaryDirectory(prefix="elision_gen_") as tmp:
            tmpd = Path(tmp)

            def one(i: int, text: str):
                src = tmpd / f"gen_{i}.tr"
                src.write_text(text)
                try:
                    return text, compare(a.compiler, src, tmpd, a.opt, a.timeout)
                finally:
                    src.unlink(missing_ok=True)

            with ThreadPoolExecutor(a.jobs) as ex:
                while time.time() < deadline:
                    batch = [Gen(rng).program() for _ in range(a.jobs * 4)]
                    for text, diff in ex.map(lambda t: one(t[0] + n, t[1]), enumerate(batch)):
                        if diff:
                            gen_findings += 1
                            report(out, f"gen_{seed}_{n}", text, *diff)
                    n += len(batch)
        findings += gen_findings
        print(f"elision_diff: generated: {n} programs (seed {seed}), {gen_findings} difference(s)")
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
