#!/usr/bin/env python3
"""crash_fuzz.py -- REL-004 stage 4: crash fuzzing of the compiler.

The bar (ROADMAP_v2.0.0.md Part VII): invalid input must produce diagnostics,
never a crash. A compiler that exits 0 or 1 on a mutated program has behaved;
anything else is a finding:

  * killed by a signal (segfault, abort -- exit status >= 128),
  * a sanitizer report (AddressSanitizer / UBSan "runtime error:"),
  * an LLVM assertion or "LLVM ERROR" / "Stack dump" / "Internal compiler error"
    / "Internal error" (what an ICE prints),
  * a hang (the wall-clock budget below),
  * exit status 2 (the command line is wrong -- the fuzzer never gives one).

Seeds are the repository's own programs (tests/, showcase/, examples/): they
already reach every feature, so a one-token change lands somewhere interesting.
Mutations are token-level (this file) plus the line-level mutators SLM-001
wrote for its benchmark (scripts/slm/slm_benchmark/mutate.py: delete a `;`, swap a
type, drop a `}`, typo an identifier), reused as-is through importlib.

A seed that imports a sibling module must still find it, so each seed
directory is mirrored once as a farm of symlinks in the scratch directory and
the mutant is written beside them. tests/99_known_bugs (documented crashers),
tests/custom, project directories (turf.toml) and `*_module.tr` companions
(no main) are not seeds.

Every finding is minimised (line-wise delta debugging that keeps the same
failure class), written to --out as <class>_<hash>.tr with a .txt report, and
printed. Re-run one with:  scripts/dev/fuzz/crash_fuzz.py --replay FILE.tr

Usage: scripts/dev/fuzz/crash_fuzz.py [--seconds N] [--seed S] [--jobs J]
           [--compiler PATH] [--out DIR] [--timeout SEC] [--opt -O0|-O2|...]
Exit:  0 = no finding; 1 = at least one finding; 2 = the harness itself failed.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
# Importing SLM-001's mutate.py must not leave a __pycache__ in the repository.
sys.dont_write_bytecode = True

# ── tokens ──────────────────────────────────────────────────────────────────
TOKEN_RE = re.compile(
    r"""(?P<ws>\s+)
      | (?P<comment>//[^\n]*)
      | (?P<str>"(?:\\.|[^"\\\n])*"?)
      | (?P<chr>'(?:\\.|[^'\\\n])*'?)
      | (?P<num>\d[\w.]*)
      | (?P<id>[A-Za-z_]\w*)
      | (?P<op>::|->|=>|==|!=|<=|>=|&&|\|\||<<|>>|\+\+|--|[-+*/%&|^]=|\.\.|.)
    """,
    re.VERBOSE | re.DOTALL,
)

PUNCT = ["(", ")", "{", "}", "[", "]", ";", ",", ".", ":", "::", "->", "=", "==", "!=",
         "<", ">", "<=", ">=", "+", "-", "*", "/", "%", "&", "|", "^", "!", "?", "~",
         "<<", ">>", "&&", "||", "@", "#", "$", "\\", "`"]
NUMBERS = ["0", "1", "-1", "255", "256", "2147483647", "2147483648", "4294967296",
           "9223372036854775807", "9223372036854775808", "99999999999999999999",
           "0x", "0xFFFFFFFFFFFFFFFF", "1e308", "1e999", "0.0", "1.", ".5", "1_000",
           "0b102", "1.2.3", "08"]
STRINGS = ['""', '"\\', '"\\q"', '"\\x"', '"\\u{110000}"', "'a'", "''", "'ab'", "'\\n'"]
TYPES = ["int", "int32", "double", "bool", "char", "string", "void", "int*", "int[]",
         "&int", "&?int", "!int"]


def keywords() -> list[str]:
    """The lexer's own keyword table, so a keyword added later is fuzzed with
    no edit here."""
    try:
        text = (REPO / "src" / "Lexer.cpp").read_text()
        words = sorted(set(re.findall(r'\{"([a-z_]+)",\s*TOK_', text)))
        if words:
            return words
    except OSError:
        pass
    return ["fn", "struct", "enum", "if", "while", "return", "import", "pub", "mut"]


def tokenize(src: str) -> list[tuple[str, str]]:
    out, pos = [], 0
    while pos < len(src):
        m = TOKEN_RE.match(src, pos)
        if not m:  # unreachable (the op arm eats any char); stay total anyway
            out.append(("op", src[pos]))
            pos += 1
            continue
        out.append((m.lastgroup, m.group()))
        pos = m.end()
    return out


def render(toks: list[tuple[str, str]]) -> str:
    return "".join(t for _, t in toks)


def code_idx(toks) -> list[int]:
    return [i for i, (k, _) in enumerate(toks) if k not in ("ws", "comment")]


def load_line_mutators():
    """SLM-001's mutators, reused (ROADMAP REL-004 stage 4)."""
    path = REPO / "scripts" / "slm" / "slm_benchmark" / "mutate.py"
    spec = importlib.util.spec_from_file_location("slm_mutate", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.MUTATORS


# ── mutation operators ──────────────────────────────────────────────────────
class Mutator:
    def __init__(self, rng: random.Random, vocab: list[str]):
        self.rng = rng
        self.kw = keywords()
        self.vocab = vocab
        self.line_mut = load_line_mutators()

    def random_token(self) -> str:
        r = self.rng
        pool = r.choice([self.kw, PUNCT, NUMBERS, STRINGS, TYPES, self.vocab or PUNCT])
        return r.choice(pool)

    def token_op(self, toks):
        r = self.rng
        idx = code_idx(toks)
        if not idx:
            return toks
        i = r.choice(idx)
        op = r.choice(["delete", "dup", "swap", "replace", "insert", "truncate",
                       "swap_ident", "number", "nest", "wrap_chain", "drop_run"])
        toks = list(toks)
        if op == "delete":
            del toks[i]
        elif op == "dup":
            toks.insert(i, toks[i])
        elif op == "swap":
            j = r.choice(idx)
            toks[i], toks[j] = toks[j], toks[i]
        elif op == "replace":
            toks[i] = ("op", self.random_token())
        elif op == "insert":
            toks[i:i] = [("op", self.random_token()), ("ws", " ")]
        elif op == "truncate":
            toks = toks[:i]
        elif op == "swap_ident":
            ids = [k for k in idx if toks[k][0] == "id"]
            if len(ids) >= 2:
                a, b = r.sample(ids, 2)
                toks[a], toks[b] = toks[b], toks[a]
        elif op == "number":
            nums = [k for k in idx if toks[k][0] == "num"]
            if nums:
                toks[r.choice(nums)] = ("num", r.choice(NUMBERS))
        elif op == "nest":
            # Depth is the classic stack-overflow / quadratic-time shape.
            opener, closer = r.choice([("(", ")"), ("{", "}"), ("[", "]"), ("-", ""), ("!", "")])
            n = r.choice([50, 300, 2000, 20000])
            toks[i:i] = [("op", opener * n + closer * n)]
        elif op == "wrap_chain":
            # Depth built from FINISHED pieces: `((x--…--)--…--)--…--`. Each
            # piece is inside the parser's per-construct limits, the tree is
            # their sum (found by hand after the fuzzer's nesting ops missed it).
            piece = r.choice(["--", "++", ".a", "[0]"])
            n, levels = r.choice([100, 700, 1500]), r.choice([2, 3, 10])
            e = toks[i][1]
            for _ in range(levels):
                e = "(" + e + piece * n + ")"
            toks[i] = (toks[i][0], e)
        elif op == "drop_run":
            j = min(len(toks), i + r.randint(2, 40))
            del toks[i:j]
        return toks

    def line_op(self, src: str) -> str:
        r = self.rng
        lines = src.splitlines(keepends=True)
        if not lines:
            return src
        kind = r.choice(["delete_line", "dup_line", "swap_lines", "delete_semicolon",
                         "swap_type", "drop_closing_brace", "typo_identifier"])
        n = r.randrange(len(lines))
        try:
            if kind == "delete_line":
                del lines[n]
            elif kind == "dup_line":
                lines.insert(n, lines[n])
            elif kind == "swap_lines":
                m = r.randrange(len(lines))
                lines[n], lines[m] = lines[m], lines[n]
            elif kind == "swap_type":
                a, b = r.sample(TYPES[:7], 2)
                lines = self.line_mut["swap_type"](lines, n + 1, {"from": a, "to": b})
            elif kind == "typo_identifier":
                ids = re.findall(r"[A-Za-z_]\w+", lines[n])
                if ids:
                    lines = self.line_mut["typo_identifier"](lines, n + 1, {"name": r.choice(ids)})
            else:
                lines = self.line_mut[kind](lines, n + 1, {})
        except (AssertionError, IndexError):
            pass  # the SLM mutators assert their precondition; a miss is a no-op
        return "".join(lines)

    def mutate(self, src: str) -> str:
        for _ in range(self.rng.choice([1, 1, 1, 2, 3, 5])):
            if self.rng.random() < 0.3:
                src = self.line_op(src)
            else:
                src = render(self.token_op(tokenize(src)))
        return src


# ── running the compiler ────────────────────────────────────────────────────
ANSI = re.compile(r"\x1b\[[0-9;]*m")
SIGNATURES = [
    ("sanitizer", re.compile(r"AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:|LeakSanitizer")),
    ("assert", re.compile(r"Assertion `|: Assertion .* failed|LLVM ERROR|PLEASE submit a bug report|Stack dump")),
    ("ice", re.compile(r"[Ii]nternal (compiler )?error|terminate called|std::bad_alloc|std::out_of_range|what\(\):")),
]


# A diagnostic quotes the input's own line (`  12 │ <source>`). A mutated input
# can carry crash text in a comment, so a quoted line is never evidence.
EXCERPT = re.compile(r"^[ \t]*\d*[ \t]*\u2502.*$", re.M)


def classify(rc: int | None, out: str) -> str | None:
    """None = the compiler behaved (diagnostics are exit 1; success is 0)."""
    if rc is None:
        return "hang"
    out = EXCERPT.sub("", out)
    for name, rx in SIGNATURES:
        if rx.search(out):
            return name
    if rc < 0:
        return f"signal{-rc}"
    if rc >= 128:
        return f"signal{rc - 128}"
    if rc not in (0, 1):
        return f"exit{rc}"
    return None


def run_compiler(compiler: str, src_path: Path, outbin: Path, opt: str, timeout: float):
    cmd = [compiler, str(src_path), "-o", str(outbin)] + ([opt] if opt else [])
    env = dict(os.environ)
    env.setdefault("ASAN_OPTIONS", "detect_leaks=0:abort_on_error=0")
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, errors="replace",
                           timeout=timeout, env=env, cwd=src_path.parent)
    except subprocess.TimeoutExpired:
        return None, "timeout"
    return p.returncode, ANSI.sub("", p.stdout + p.stderr)


# ── seed farms ──────────────────────────────────────────────────────────────
def seeds() -> list[Path]:
    out = []
    for root in ("tests", "showcase", "examples"):
        for p in sorted((REPO / root).rglob("*.tr")):
            rel = p.relative_to(REPO).parts
            if rel[:2] in (("tests", "99_known_bugs"), ("tests", "custom"), ("tests", "api")):
                continue
            if p.name.endswith("_module.tr") or (p.parent / "turf.toml").exists():
                continue
            if p.stat().st_size > 60_000:
                continue
            out.append(p)
    return out


class Farms:
    """One symlink mirror per seed directory, created lazily and shared."""

    def __init__(self, base: Path):
        self.base, self.made, self.lock = base, {}, threading.Lock()

    def dir_for(self, seed: Path, worker: int) -> Path:
        key = seed.parent
        with self.lock:
            if key not in self.made:
                d = self.base / f"farm{len(self.made)}"
                d.mkdir()
                for f in key.iterdir():
                    if f.is_file():
                        (d / f.name).symlink_to(f)
                self.made[key] = d
            return self.made[key]


# ── minimisation ────────────────────────────────────────────────────────────
def minimise(compiler, farm: Path, name: str, src: str, klass: str, opt: str, timeout: float) -> str:
    """Delta-debug by lines, keeping only reductions that fail the same way.
    Bounded twice: a hang costs `timeout` per probe, so there is a probe cap and
    a wall-clock cap (a finding must be reported while the run is still alive,
    not after a quarter of an hour of reducing it)."""
    p = farm / name
    outbin = farm / (name + ".bin")
    probes = [0]
    give_up = time.time() + 180

    def same(candidate: str) -> bool:
        if probes[0] >= 400 or time.time() > give_up:
            return False
        probes[0] += 1
        p.write_text(candidate)
        rc, out = run_compiler(compiler, p, outbin, opt, timeout)
        return classify(rc, out) == klass

    lines = src.splitlines(keepends=True)
    n = 2
    while len(lines) >= 2 and n <= len(lines):
        size = max(1, len(lines) // n)
        reduced = False
        for start in range(0, len(lines), size):
            cand = lines[:start] + lines[start + size:]
            if cand and same("".join(cand)):
                lines, n, reduced = cand, max(n - 1, 2), True
                break
        if not reduced:
            if size == 1:
                break
            n = min(len(lines), n * 2)
    return "".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seconds", type=float, default=60, help="wall-clock budget (default 60)")
    ap.add_argument("--seed", type=int, default=None, help="RNG seed (default: time-based, printed)")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    ap.add_argument("--compiler", default=os.environ.get("TURF_COMPILER", str(REPO / "build" / "turfc")))
    ap.add_argument("--out", default=str(REPO / "fuzz-findings"))
    ap.add_argument("--timeout", type=float, default=20, help="per-compile hang bound (default 20s)")
    ap.add_argument("--opt", default="", help="flag passed through, e.g. -O0 or -g; \"random\" picks one per compile")
    ap.add_argument("--replay", help="compile one file and classify it; no fuzzing")
    ap.add_argument("--max-iterations", type=int, default=0, help="stop after N compiles (0 = time only)")
    args = ap.parse_args()

    if not os.access(args.compiler, os.X_OK):
        print(f"fuzz: compiler {args.compiler} not built", file=sys.stderr)
        return 2

    if args.replay:
        src = Path(args.replay).resolve()
        rc, out = run_compiler(args.compiler, src, Path(tempfile.mkdtemp()) / "a.out", args.opt, args.timeout)
        k = classify(rc, out)
        print(f"{src}: {k or 'ok (exit %s)' % rc}")
        if k:
            print(out[-2000:])
        return 1 if k else 0

    pool = seeds()
    if not pool:
        print("fuzz: no seeds found", file=sys.stderr)
        return 2
    seed = args.seed if args.seed is not None else int(time.time())
    print(f"fuzz: {len(pool)} seeds, {args.jobs} jobs, {args.seconds:.0f}s, seed {seed}, compiler {args.compiler}")

    vocab = sorted({w for p in pool[:300] for k, w in tokenize(p.read_text(errors="replace")) if k == "id"})[:3000]
    base = Path(tempfile.mkdtemp(prefix="turf_fuzz_"))
    farms = Farms(base)
    out_dir = Path(args.out)
    deadline = time.time() + args.seconds
    stats = {"runs": 0, "ok": 0, "diag": 0}
    findings: dict[str, Path] = {}
    lock = threading.Lock()

    def worker(w: int):
        rng = random.Random(seed * 1000 + w)
        mut = Mutator(rng, vocab)
        while time.time() < deadline:
            if args.max_iterations and stats["runs"] >= args.max_iterations:
                return
            sp = rng.choice(pool)
            # "random" spreads the optimizer and the debug-info path across
            # the run: a crash in -O3 or -g code generation needs a program
            # that gets that far, which the default level never asks for.
            opt = rng.choice(["-O0", "-O1", "-O2", "-O3", "-Os", "-g"]) if args.opt == "random" else args.opt
            src = mut.mutate(sp.read_text(errors="replace"))
            farm = farms.dir_for(sp, w)
            name = f"__fuzz_w{w}.tr"
            (farm / name).write_text(src)
            rc, out = run_compiler(args.compiler, farm / name, farm / f"__fuzz_w{w}.bin", opt, args.timeout)
            klass = classify(rc, out)
            with lock:
                stats["runs"] += 1
                stats["ok" if rc == 0 else "diag"] += 0 if klass else 1
            if klass is None:
                continue
            if klass == "hang":
                # A seed that is itself slow (the E079 fan-out test takes ~20s)
                # is a slow test, not a fuzz finding: only keep a hang the
                # unmutated seed does not have.
                rc0, _ = run_compiler(args.compiler, farm / sp.name, farm / f"__fuzz_w{w}.bin", opt, args.timeout)
                if rc0 is None:
                    continue
            digest = hashlib.sha1(src.encode()).hexdigest()[:10]
            with lock:
                key = f"{klass}_{digest}"
                if key in findings:
                    continue
                findings[key] = Path()
            small = minimise(args.compiler, farm, name, src, klass, opt, args.timeout)
            rc2, out2 = run_compiler(args.compiler, farm / name, farm / f"__fuzz_w{w}.bin", opt, args.timeout)
            out_dir.mkdir(parents=True, exist_ok=True)
            # Content-addressed by the MINIMISED input so two seeds reducing to
            # one bug report one file.
            sig = hashlib.sha1(small.encode()).hexdigest()[:10]
            target = out_dir / f"{klass}_{sig}.tr"
            target.write_text(small)
            target.with_suffix(".txt").write_text(
                f"class: {klass}\nseed file: {sp.relative_to(REPO)}\nfuzz seed: {seed} worker {w}\n"
                f"compiler: {args.compiler} {opt}\nexit: {rc}\n"
                f"reproduce: scripts/dev/fuzz/crash_fuzz.py --replay {target} --compiler {args.compiler} {('--opt ' + opt) if opt else ''}\n"
                f"note: compile it from a copy beside the seed's sibling modules if it imports any.\n"
                f"--- compiler output (tail) ---\n{(out2 or out)[-3000:]}\n")
            with lock:
                findings[key] = target
            print(f"FINDING [{klass}] {target}  (from {sp.relative_to(REPO)})", flush=True)

    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        for f in [ex.submit(worker, w) for w in range(args.jobs)]:
            f.result()
    shutil.rmtree(base, ignore_errors=True)

    real = [p for p in findings.values() if str(p) != "."]
    print(f"fuzz: {stats['runs']} compiles, {len(real)} finding(s), seed {seed}")
    return 1 if real else 0


if __name__ == "__main__":
    sys.exit(main())
