#!/usr/bin/env python3
"""The 2.0 compatibility corpus (V-4, ROADMAP_v3.0.0.md 5.5.1, TDP-2-003).

tests/compat/2.0/ is the 2.0.0 test tree, copied from the v2.0.0 tag, with the
output each program that compiled on 2.0.0 gave then. Every 2.x compiler must
still accept those programs and print the same bytes: a minor that rejects or
changes one has broken a program that compiled on 2.0, which only a major may
do. This script is the CI gate for that. It fails when

  1. the corpus was edited. A file changed, added or removed against
     SHA256SUMS: a minor that edits a test to make it pass is a 3.0 change.
     Add a new test under tests/<NN_topic>/, never here;
  2. `turfc --test` fails on the tree (a program that compiled stops
     compiling, a rejected one is accepted, a marker no longer holds);
  3. a program that compiled and ran on 2.0.0 exits with another status or
     prints other bytes (RUNS lists the programs and their exit statuses;
     <program>.tr.expected holds the output, absent when it was empty);
  4. a RELAXED.txt entry is stale: it names a program 2.0.0 rejected that
     this compiler still rejects (the line must go), or one that is not in
     the corpus.

RELAXED.txt is the one register of exceptions. A program 2.0.0 REJECTED that a
later minor deliberately accepts is not a program that stopped compiling, so
it does not break the promise, but it is still a change to what the corpus
says, so it is written down there with the commit and the reason, and the
owner reviews the line. It can only name a fail_* program (a program that
compiled on 2.0.0 is never an exception) and only ever shrinks or grows by
review.

Output is compared byte for byte (stdout and stderr in one stream, as
`turfc --test` sees it) except for one thing: the line number in a panic's
`<...>/lib/std/<file>.tr:<line>:` location is the standard library's own source
line, which moves whenever lib/std is edited, so both sides are normalised to
`<std>/<file>.tr:<line>:`.

Usage: scripts/check/compat_corpus_check.py [-O0|-O1|-O2|-O3|-Os] [-g] [--compiler PATH]
       scripts/check/compat_corpus_check.py --record --compiler <turfc 2.0.0>

--record (re)writes SHA256SUMS, RUNS and the .expected files by running the
programs with a 2.0.0 compiler; it refuses any other compiler, so the corpus
can never be re-blessed with the compiler it is meant to judge. Its use is to
prove provenance: with the corpus copied unedited from `git archive v2.0.0
tests`, `--record` with a 2.0.0 `turfc` must leave `git status` clean.

Run from anywhere. Exit: 0 = the corpus holds; 1 = at least one problem.
Environment: TURF_TEST_JOBS (parallel programs, default every processor),
TURF_TEST_TIMEOUT_SEC (one program's run, default 20; the corpus's own
`turfc --test` pass keeps the runner's default of 5 unless this is set).
"""
from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import os
import re
import signal
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
CORPUS = REPO / "tests" / "compat" / "2.0"
SUMS = CORPUS / "SHA256SUMS"
RUNS = CORPUS / "RUNS"
RELAXED = CORPUS / "RELAXED.txt"
COMPILE_TIMEOUT_SEC = 300

# Files in the corpus directory that are not the frozen tree.
OWN_FILES = {"SHA256SUMS", "RUNS", "RELAXED.txt", "README.md"}

STD_LOCATION = re.compile(rb"(?:[^\s:]*/)?lib/std/([^\s:]+):\d+:")


def normalise(out: bytes) -> bytes:
    return STD_LOCATION.sub(lambda m: b"<std>/" + m.group(1) + b":<line>:", out)


def sha256(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def frozen_files() -> list[Path]:
    """Every file of the frozen tree: the programs and their .expected output."""
    return sorted(
        p for p in CORPUS.rglob("*")
        if p.is_file() and p.relative_to(CORPUS).as_posix() not in OWN_FILES)


def rel(p: Path) -> str:
    return p.relative_to(CORPUS).as_posix()


def read_sums() -> dict[str, str]:
    sums = {}
    for line in SUMS.read_text().splitlines():
        digest, _, name = line.partition("  ")
        if digest and name:
            sums[name] = digest
    return sums


def read_runs() -> dict[str, int]:
    runs = {}
    for line in RUNS.read_text().splitlines():
        status, _, name = line.partition(" ")
        if status and name:
            runs[name] = int(status)
    return runs


def read_relaxed() -> dict[str, str]:
    out = {}
    for n, line in enumerate(RELAXED.read_text().splitlines(), 1):
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        name, _, why = s.partition("#")
        name, why = name.strip(), why.strip()
        if not why:
            sys.exit(f"{RELAXED}:{n}: an entry needs `# <commit>: <reason>` after the path")
        out[name] = why
    return out


def programs() -> list[Path]:
    """What `turfc --test` would discover: every .tr that is not a companion."""
    return sorted(p for p in CORPUS.rglob("*.tr") if not p.name.endswith("_module.tr"))


def exit_status(returncode: int) -> int:
    """A signal is reported the way a shell does: SIGABRT is 134."""
    return 128 - returncode if returncode < 0 else returncode


def run_group(cmd: list[str], cwd: Path, timeout: int):
    """Run cmd with stdin from /dev/null, stdout and stderr in one stream, in its
    own process group; on a timeout kill the whole group (a program that leaves
    a child holding the pipe would otherwise block us, as it would block the
    runner). (status, output); a timeout is status 124."""
    proc = subprocess.Popen(cmd, cwd=cwd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, start_new_session=True)
    try:
        out, _ = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        out, _ = proc.communicate()
        return 124, b"<timed out after %ds>" % timeout
    return exit_status(proc.returncode), out


def run_program(compiler: str, flags: list[str], p: Path, tmp: Path, index: int,
                run_timeout: int, repeat: int):
    """Compile one program once and run it `repeat` times. (None, output) if it
    does not compile, else a list of (exit status, normalised output)."""
    exe = tmp / f"p{index}"
    status, out = run_group([compiler, p.name, "-o", str(exe), *flags], p.parent, COMPILE_TIMEOUT_SEC)
    if status != 0:
        return None, out
    runs = []
    for _ in range(repeat):
        status, out = run_group([str(exe)], p.parent, run_timeout)
        runs.append((status, normalise(out)))
    return runs


def run_all(compiler: str, flags: list[str], progs: list[Path], jobs: int,
            run_timeout: int, repeat: int = 1):
    """{program: [(status, output)] * repeat, or (None, compiler output)}"""
    with tempfile.TemporaryDirectory(prefix="turf-compat-") as t:
        tmp = Path(t)
        with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
            futs = {p: pool.submit(run_program, compiler, flags, p, tmp, i, run_timeout, repeat)
                    for i, p in enumerate(progs)}
            return {p: f.result() for p, f in futs.items()}


def compiler_version(compiler: str) -> str:
    out = subprocess.run([compiler, "--version"], capture_output=True, text=True).stdout
    return out.splitlines()[0].strip() if out else ""


# ── --record ────────────────────────────────────────────────────────────────

def record(compiler: str, jobs: int, run_timeout: int) -> int:
    version = compiler_version(compiler)
    if version != "turfc 2.0.0":
        print(f"--record refuses `{version or compiler}`: the corpus is what a 2.0.0 "
              "compiler did, and only a 2.0.0 `turfc` may say so.", file=sys.stderr)
        return 1
    for p in CORPUS.rglob("*.expected"):
        p.unlink()
    relaxed = read_relaxed() if RELAXED.exists() else {}
    progs = [p for p in programs() if rel(p) not in relaxed]
    # Run each program twice: output that differs between two runs is not
    # something a later compiler can be held to.
    results = run_all(compiler, ["--allow-unsafe-modules"], progs, jobs, run_timeout, repeat=2)
    runs, unstable = [], []
    for p in progs:
        res = results[p]
        if isinstance(res, tuple):
            continue  # rejected by 2.0.0: the runner's fail_* rule covers it
        (status, out), again = res
        if (status, out) != again:
            unstable.append(rel(p))
        runs.append(f"{status} {rel(p)}")
        if out:
            (p.parent / (p.name + ".expected")).write_bytes(out)
    if unstable:
        print("output differs between two runs, so it cannot be pinned:", file=sys.stderr)
        for u in unstable:
            print(f"  {u}", file=sys.stderr)
        return 1
    RUNS.write_text("".join(line + "\n" for line in sorted(runs, key=lambda l: l.split(" ", 1)[1])))
    SUMS.write_text("".join(f"{sha256(p)}  {rel(p)}\n" for p in frozen_files()))
    print(f"recorded {len(runs)} programs, {len(frozen_files())} frozen files")
    return 0


# ── check ───────────────────────────────────────────────────────────────────

def check_frozen(problems: list[str]) -> None:
    sums = read_sums()
    present = {rel(p): p for p in frozen_files()}
    for name, digest in sums.items():
        if name not in present:
            problems.append(f"removed: {name} is in SHA256SUMS but not in the corpus")
        elif sha256(present[name]) != digest:
            problems.append(f"edited: {name} no longer matches SHA256SUMS")
    for name in present:
        if name not in sums:
            problems.append(f"added: {name} is not in SHA256SUMS")


def check_runner(compiler: str, flags: list[str], relaxed: dict[str, str],
                 problems: list[str]) -> None:
    cmd = [compiler, "--test", str(CORPUS)]
    for name in relaxed:
        cmd += ["--exclude", name]
    r = subprocess.run(cmd + flags, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True, errors="replace")
    if r.returncode != 0:
        tail = re.sub(r"\x1b\[[0-9;]*m", "", r.stdout)
        failed = tail.split("Failed Tests Details:", 1)[-1] if "Failed Tests Details:" in tail else tail[-4000:]
        problems.append("`turfc --test tests/compat/2.0` failed:\n" + failed.rstrip())
    else:
        total = re.search(r"Total Tests: (\d+)", r.stdout)
        print(f"  runner: {total.group(1) if total else '?'} programs, all as 2.0.0 had them")


def check_relaxed(compiler: str, relaxed: dict[str, str], problems: list[str]) -> None:
    for name in relaxed:
        p = CORPUS / name
        if not p.is_file() or not p.name.startswith("fail_") or name in read_runs():
            problems.append(f"RELAXED.txt: {name} is not a fail_* program of the corpus")
            continue
        r = subprocess.run([compiler, p.name, "--check"], cwd=p.parent,
                           stdin=subprocess.DEVNULL, capture_output=True)
        if r.returncode != 0:
            problems.append(f"RELAXED.txt: {name} is still rejected, so it is not a relaxation; "
                            "delete the line")


def check_outputs(compiler: str, flags: list[str], jobs: int, run_timeout: int,
                  problems: list[str]) -> None:
    runs = read_runs()
    progs = [CORPUS / name for name in sorted(runs)]
    results = run_all(compiler, flags, progs, jobs, run_timeout)
    broken = 0
    for p in progs:
        name = rel(p)
        res = results[p]
        if isinstance(res, tuple):
            problems.append(f"no longer compiles: {name}\n" + res[1].decode(errors="replace")[-1500:])
            broken += 1
            continue
        status, out = res[0]
        want_file = p.parent / (p.name + ".expected")
        want = want_file.read_bytes() if want_file.exists() else b""
        if status != runs[name]:
            problems.append(f"exit status changed: {name}: was {runs[name]}, now {status}")
            broken += 1
        elif out != want:
            problems.append(f"output changed: {name}\n--- 2.0.0\n{want.decode(errors='replace')}"
                            f"--- now\n{out.decode(errors='replace')}")
            broken += 1
    print(f"  output: {len(progs) - broken} of {len(progs)} programs print what 2.0.0 printed")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--compiler", default=os.environ.get("TURF_COMPILER") or str(REPO / "build" / "turfc"))
    ap.add_argument("--record", action="store_true")
    args, flags = ap.parse_known_args()
    args.flags = flags
    for f in args.flags:
        if f not in ("-O0", "-O1", "-O2", "-O3", "-Os", "-g"):
            ap.error(f"unknown argument {f!r} (expected -O0|-O1|-O2|-O3|-Os and/or -g)")
    args.compiler = str(Path(args.compiler).resolve())  # programs run in their own directory
    if not Path(args.compiler).is_file():
        print(f"Compiler not found at {args.compiler}. Please build Turf first.", file=sys.stderr)
        return 1
    jobs = int(os.environ.get("TURF_TEST_JOBS") or os.cpu_count() or 2)
    run_timeout = int(os.environ.get("TURF_TEST_TIMEOUT_SEC") or 20)

    if args.record:
        return record(args.compiler, jobs, run_timeout)

    problems: list[str] = []
    print("tests/compat/2.0: the 2.0.0 test tree and what it printed")
    check_frozen(problems)
    if problems:
        # An edited corpus proves nothing about the compiler; say so first.
        print("\nThe corpus is immutable (ROADMAP_v3.0.0.md 5.5.1): a minor that edits a test "
              "to make it pass is a 3.0 change. A new test goes under tests/<NN_topic>/.\n")
        print("\n".join(problems))
        return 1
    relaxed = read_relaxed()
    check_relaxed(args.compiler, relaxed, problems)
    check_runner(args.compiler, args.flags, relaxed, problems)
    check_outputs(args.compiler, args.flags, jobs, run_timeout, problems)
    if problems:
        print(f"\n{len(problems)} problem(s): a program that compiled on 2.0.0 no longer "
              "compiles or no longer prints the same. A minor cannot do that (V-4).\n")
        print("\n".join(problems))
        return 1
    print(f"  relaxed: {len(relaxed)} program(s) 2.0.0 rejected that are now accepted, on the register")
    print("compat corpus holds")
    return 0


if __name__ == "__main__":
    sys.exit(main())
