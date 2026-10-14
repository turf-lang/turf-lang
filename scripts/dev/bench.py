#!/usr/bin/env python3
"""REL-005: the performance baseline.

Builds every bench/*.tr at -O2 and runs it --runs times, recording the median
and minimum wall time, peak memory (Linux) and the checksum it prints (it must
not change between runs). A bench/NAME.c twin is built with `clang -O3` and
reported as Turf's time over C's. Two compile-time numbers: checking every
lib/std module (`--check --module`) and building every showcase/*.tr at -O2.

    python3 scripts/dev/bench.py [--turfc PATH] [--runs N] [--only NAME]
                             [--cpu N] [--out FILE.json] [--compare BASE.json]
                             [--threshold 0.20]

--out writes the results as JSON; the Markdown table printed at the end is
the row block docs/development/BENCHMARKS.md records per milestone.
--compare BASE.json is the release gate (REL-007 checklist): it exits 1 when
any benchmark's minimum time is more than --threshold slower than BASE's.
Compare only numbers taken on the same machine with the same kind of build;
the script warns when the recorded machine or build type differ.

Two things keep run-to-run noise under the gate, both measured on the
baseline machine (a laptop i5-13500H under WSL): every timed command is
pinned to one CPU (`taskset -c 2` on Linux; --cpu -1 turns it off), and the
runs are interleaved -- round r runs every benchmark once -- so turbo and
thermal drift over the session lands on all of them alike instead of on
whichever ran last. Unpinned and back to back, the same commit compared
against itself moved up to 30%. Still: run it on an idle machine.
"""
import argparse
import json
import os
import platform
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BENCH = os.path.join(ROOT, "bench")

# GNU time's %M: the peak RSS of the program alone. Measuring it from here
# (os.wait4's ru_maxrss) is wrong: the child is forked from this Python
# process and the peak carries across exec, so every program "used" ~16 MB.
GNU_TIME = "/usr/bin/time" if sys.platform.startswith("linux") and \
    os.access("/usr/bin/time", os.X_OK) else None
PIN = []  # ["taskset", "-c", N] once main() has decided


def run_timed(cmd, cwd=None):
    """(wall seconds, peak RSS in KB or None, stdout, exit status)."""
    rss_file = None
    cmd = PIN + cmd
    if GNU_TIME:
        fd, rss_file = tempfile.mkstemp(prefix="turf_bench_rss_")
        os.close(fd)
        cmd = [GNU_TIME, "-f", "%M", "-o", rss_file] + cmd
    start = time.perf_counter()
    p = subprocess.run(cmd, cwd=cwd, capture_output=True)
    wall = time.perf_counter() - start
    rss = None
    if rss_file:
        try:
            with open(rss_file) as f:
                rss = int(f.read().strip().splitlines()[-1])
        except (OSError, ValueError, IndexError):
            pass
        os.unlink(rss_file)
    return wall, rss, (p.stdout + p.stderr).decode(errors="replace"), p.returncode


def summarize(walls):
    return {"median_s": round(statistics.median(walls), 4),
            "min_s": round(min(walls), 4),
            "runs_s": [round(w, 4) for w in walls]}


def build_type(turfc):
    """The CMAKE_BUILD_TYPE of the build turfc came from, if it can be read."""
    cache = os.path.join(os.path.dirname(os.path.abspath(turfc)), "CMakeCache.txt")
    try:
        with open(cache) as f:
            for line in f:
                if line.startswith("CMAKE_BUILD_TYPE:"):
                    return line.split("=", 1)[1].strip() or "unset"
    except OSError:
        pass
    return "unknown"


def machine():
    cpu = platform.processor() or platform.machine()
    try:
        with open("/proc/cpuinfo") as f:
            for line in f:
                if line.startswith("model name"):
                    cpu = line.split(":", 1)[1].strip()
                    break
    except OSError:
        pass
    return {"cpu": cpu, "cores": os.cpu_count(), "os": platform.platform()}


def meta(turfc, pin):
    version = subprocess.run([turfc, "--version"], capture_output=True, text=True).stdout
    commit = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"],
                            capture_output=True, text=True).stdout.strip()
    # What the numbers depend on; bench/results/ holds the numbers themselves.
    dirty = subprocess.run(["git", "-C", ROOT, "status", "--porcelain", "--", "src",
                            "include", "lib", "runtime", "bench",
                            ":(exclude)bench/results"],
                           capture_output=True, text=True).stdout.strip() != ""
    return {"date": time.strftime("%Y-%m-%d"), "commit": commit + ("+dirty" if dirty else ""),
            "turfc": os.path.relpath(os.path.abspath(turfc), ROOT),
            "build_type": build_type(turfc),
            "turfc_version": " / ".join(l for l in version.splitlines() if l),
            "pinned_cpu": pin, "machine": machine()}


class Job:
    """One measured thing: `run()` is one timed repetition."""

    def __init__(self, name, run):
        self.name, self.run = name, run
        self.walls, self.rss, self.outs, self.error = [], [], set(), None


def program_jobs(turfc, src, work):
    """Jobs for one bench/*.tr: the program and, if present, its C twin."""
    name = os.path.splitext(os.path.basename(src))[0]
    exe = os.path.join(work, name)
    r = subprocess.run([turfc, "-O2", src, "-o", exe], capture_output=True, text=True, cwd=work)
    if r.returncode != 0:
        j = Job(name, None)
        j.error = "compile failed: " + (r.stdout + r.stderr).strip()[-300:]
        return [j]
    jobs = [Job(name, lambda: run_timed([exe], cwd=work))]
    c_src = os.path.join(BENCH, name + ".c")
    clang = shutil.which("clang")
    if os.path.exists(c_src) and clang:
        c_exe = exe + "_c"
        if subprocess.run([clang, "-O3", c_src, "-o", c_exe]).returncode == 0:
            jobs.append(Job(name + "#c", lambda: run_timed([c_exe], cwd=work)))
    return jobs


def compile_jobs(turfc, work):
    """Compile-time jobs: lib/std checked, showcase/ built at -O2."""
    std = sorted(os.path.join(d, f) for d, _, fs in os.walk(os.path.join(ROOT, "lib", "std"))
                 for f in fs if f.endswith(".tr"))
    show = sorted(os.path.join(ROOT, "showcase", f)
                  for f in os.listdir(os.path.join(ROOT, "showcase")) if f.endswith(".tr"))

    def make(files, flags):
        def run():
            total = 0.0
            for f in files:
                cmd = [turfc, f] + flags
                if "-O2" in flags:
                    cmd += ["-o", os.path.join(work, "showcase_bin")]
                w, _, text, code = run_timed(cmd, cwd=work)
                if code != 0:
                    return total, None, "%s failed: %s" % (
                        os.path.relpath(f, ROOT), text.strip()[-200:]), code
                total += w
            return total, None, "%d files" % len(files), 0
        return run

    return [Job("compile_stdlib_check", make(std, ["--check", "--module"])),
            Job("compile_showcase", make(show, ["-O2"]))]


def measure(jobs, runs):
    """Round r runs every job once (interleaved, see the module docstring)."""
    for r in range(runs):
        print("== round %d/%d" % (r + 1, runs), flush=True)
        for j in jobs:
            if j.error or not j.run:
                continue
            w, m, out, code = j.run()
            if code != 0:
                j.error = "exit %s: %s" % (code, out.strip()[-200:])
                continue
            j.walls.append(w)
            if m is not None:
                j.rss.append(m)
            j.outs.add(out.strip())


def results_of(jobs):
    res = {}
    by_name = {j.name: j for j in jobs}
    for j in jobs:
        if j.name.endswith("#c"):
            continue
        r = {"error": j.error} if j.error else {}
        if j.walls:
            r.update(summarize(j.walls))
            r["rss_kb"] = max(j.rss) if j.rss else None
            if len(j.outs) == 1:
                r["checksum"] = next(iter(j.outs))
            else:
                r["error"] = "output changed between runs"
            c = by_name.get(j.name + "#c")
            if c and c.walls:
                r["c_median_s"] = round(statistics.median(c.walls), 4)
                r["ratio_vs_c"] = round(r["median_s"] / r["c_median_s"], 3)
                if c.outs != j.outs:
                    r["error"] = "checksum differs from bench/%s.c" % j.name
        if j.name.startswith("compile_") and "checksum" in r:
            r["files"] = int(r.pop("checksum").split()[0])
        res[j.name] = r
    return res


def table(results):
    rows = ["| Benchmark | Median (s) | Min (s) | Peak RSS (KB) | vs C | Checksum |",
            "|---|---|---|---|---|---|"]
    for name, r in results.items():
        if "median_s" not in r:
            rows.append("| %s | error: %s | | | | |" % (name, r.get("error")))
            continue
        rows.append("| %s | %.3f | %.3f | %s | %s | %s |" % (
            name, r["median_s"], r["min_s"], r.get("rss_kb") or "",
            ("%.2fx" % r["ratio_vs_c"]) if "ratio_vs_c" in r else "",
            r.get("checksum") or ("%d files" % r["files"] if "files" in r else "")))
    return "\n".join(rows)


def compare(results, base, threshold):
    """Compares MINIMUM times: the run least disturbed by anything else on the
    machine (medians moved more than the gate between back-to-back runs)."""
    bad = []
    for name, r in results.items():
        b = base["results"].get(name)
        if not b or "min_s" not in r or "min_s" not in b:
            continue
        change = r["min_s"] / b["min_s"] - 1.0
        mark = "REGRESSION" if change > threshold else "ok"
        print("  %-22s %8.3f s -> %8.3f s  %+6.1f%%  %s" % (
            name, b["min_s"], r["min_s"], change * 100, mark))
        if change > threshold:
            bad.append(name)
    return bad


def main():
    global PIN
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--turfc", default=os.path.join(ROOT, "build_release", "turfc"),
                    help="compiler to measure (default build_release/turfc)")
    ap.add_argument("--runs", type=int, default=9)
    ap.add_argument("--only", help="only benchmarks whose name contains this")
    ap.add_argument("--cpu", type=int, default=2,
                    help="pin to this CPU with taskset (Linux; -1: don't pin)")
    ap.add_argument("--out", help="write the results as JSON here")
    ap.add_argument("--compare", help="a previous --out file: exit 1 on a regression")
    ap.add_argument("--threshold", type=float, default=0.20)
    a = ap.parse_args()
    a.turfc = os.path.abspath(a.turfc)  # compiles run inside a temp directory
    if not os.access(a.turfc, os.X_OK):
        sys.exit("no compiler at %s (build build_release, or pass --turfc)" % a.turfc)
    pin = None
    if a.cpu >= 0 and sys.platform.startswith("linux") and shutil.which("taskset"):
        PIN, pin = ["taskset", "-c", str(a.cpu)], a.cpu

    info = meta(a.turfc, pin)
    if info["build_type"] not in ("Release", "RelWithDebInfo"):
        print("warning: %s is a %s build; compile-time numbers are not comparable "
              "to a Release build's" % (info["turfc"], info["build_type"]), file=sys.stderr)
    with tempfile.TemporaryDirectory(prefix="turf_bench_") as work:
        jobs = []
        for src in sorted(os.listdir(BENCH)):
            if src.endswith(".tr") and (not a.only or a.only in src[:-3]):
                jobs += program_jobs(a.turfc, os.path.join(BENCH, src), work)
        if not a.only or "compile" in a.only:
            jobs += compile_jobs(a.turfc, work)
        measure(jobs, a.runs)
        results = results_of(jobs)

    doc = {"meta": info, "runs": a.runs, "results": results}
    if a.out:
        with open(a.out, "w") as f:
            json.dump(doc, f, indent=2)
            f.write("\n")
    m = info["machine"]
    print("\n%s, commit %s, %s build, %s (%s cores), %s, pinned to CPU %s\n" % (
        info["date"], info["commit"], info["build_type"], m["cpu"], m["cores"], m["os"], pin))
    print(table(results))

    failed = [n for n, r in results.items() if "error" in r]
    if a.compare:
        with open(a.compare) as f:
            base = json.load(f)
        if base["meta"].get("machine") != info["machine"]:
            print("\nwarning: BASE was measured on a different machine", file=sys.stderr)
        if base["meta"].get("build_type") != info["build_type"]:
            print("\nwarning: BASE used a %s build" % base["meta"].get("build_type"),
                  file=sys.stderr)
        print("\nagainst %s (%s, commit %s), threshold +%d%%:" % (
            a.compare, base["meta"]["date"], base["meta"]["commit"], a.threshold * 100))
        bad = compare(results, base, a.threshold)
        if bad:
            print("\n%d regression(s) over %d%%: %s. Each needs a recorded explanation "
                  "in docs/development/BENCHMARKS.md (REL-005)." % (
                      len(bad), a.threshold * 100, ", ".join(bad)))
            sys.exit(1)
    if failed:
        sys.exit("\nfailed: " + ", ".join(failed))


if __name__ == "__main__":
    main()
