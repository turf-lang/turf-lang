#!/usr/bin/env python3
"""REL-005: the performance baseline.

Builds every bench/*.tr at -O2 and runs it --runs times, recording the median
and minimum wall time, peak memory (POSIX) and the checksum it prints (it must
not change between runs). A bench/NAME.c twin is built with `clang -O3` and
reported as Turf's time over C's. Two compile-time numbers: checking every
lib/std module (`--check --module`) and building every showcase/*.tr at -O2.

    python3 scripts/bench.py [--turfc PATH] [--runs N] [--only NAME]
                             [--out FILE.json] [--compare BASE.json]
                             [--threshold 0.20]

--out writes the results as JSON; the Markdown table printed at the end is
the row block docs/development/BENCHMARKS.md records per milestone.
--compare BASE.json is the release gate (REL-007 checklist): it exits 1 when
any benchmark's median is more than --threshold slower than BASE's. Compare
only numbers taken on the same machine with the same kind of build; the
script warns when the recorded machine or build type differ.

Run it on an idle machine: anything else running (a fuzz job, a build) moves
the numbers by more than the gate's 20%.
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

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BENCH = os.path.join(ROOT, "bench")


# GNU time's %M: the peak RSS of the program alone. Measuring it from here
# (os.wait4's ru_maxrss) is wrong: the child is forked from this Python
# process and the peak carries across exec, so every program "used" ~16 MB.
GNU_TIME = "/usr/bin/time" if sys.platform.startswith("linux") and \
    os.access("/usr/bin/time", os.X_OK) else None


def run_timed(cmd, cwd=None):
    """(wall seconds, peak RSS in KB or None, stdout, exit status)."""
    rss_file = None
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
    return wall, rss, p.stdout.decode(errors="replace"), p.returncode


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


def meta(turfc):
    version = subprocess.run([turfc, "--version"], capture_output=True, text=True).stdout
    commit = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"],
                            capture_output=True, text=True).stdout.strip()
    dirty = subprocess.run(["git", "-C", ROOT, "status", "--porcelain", "--", "src",
                            "include", "lib", "runtime", "bench"],
                           capture_output=True, text=True).stdout.strip() != ""
    return {"date": time.strftime("%Y-%m-%d"), "commit": commit + ("+dirty" if dirty else ""),
            "turfc": os.path.relpath(os.path.abspath(turfc), ROOT),
            "build_type": build_type(turfc),
            "turfc_version": " / ".join(l for l in version.splitlines() if l),
            "machine": machine()}


def bench_program(turfc, src, runs, work):
    name = os.path.splitext(os.path.basename(src))[0]
    exe = os.path.join(work, name)
    r = subprocess.run([turfc, "-O2", src, "-o", exe], capture_output=True, text=True, cwd=work)
    if r.returncode != 0:
        return {"error": "compile failed: " + (r.stdout + r.stderr).strip()[-300:]}
    walls, rss, sums = [], [], set()
    for _ in range(runs):
        w, m, out, code = run_timed([exe], cwd=work)
        if code != 0:
            return {"error": "exit %s: %s" % (code, out.strip()[-200:])}
        walls.append(w)
        if m is not None:
            rss.append(m)
        sums.add(out.strip())
    res = summarize(walls)
    res["rss_kb"] = max(rss) if rss else None
    res["checksum"] = sums.pop() if len(sums) == 1 else None
    if len(sums):  # more than one distinct output
        res["error"] = "checksum changed between runs"
    c_src = os.path.join(BENCH, name + ".c")
    clang = shutil.which("clang")
    if os.path.exists(c_src) and clang:
        c_exe = exe + "_c"
        if subprocess.run([clang, "-O3", c_src, "-o", c_exe]).returncode == 0:
            c_walls, c_sums = [], set()
            for _ in range(runs):
                w, _, out, _ = run_timed([c_exe], cwd=work)
                c_walls.append(w)
                c_sums.add(out.strip())
            res["c_median_s"] = round(statistics.median(c_walls), 4)
            res["ratio_vs_c"] = round(res["median_s"] / res["c_median_s"], 3)
            if c_sums != {res["checksum"]}:
                res["error"] = "checksum differs from %s" % os.path.basename(c_src)
    return res


def bench_compile(turfc, runs, work):
    """Compile-time numbers: lib/std checked, showcase/ built."""
    std = sorted(os.path.join(d, f) for d, _, fs in os.walk(os.path.join(ROOT, "lib", "std"))
                 for f in fs if f.endswith(".tr"))
    show = sorted(os.path.join(ROOT, "showcase", f)
                  for f in os.listdir(os.path.join(ROOT, "showcase")) if f.endswith(".tr"))
    out = {}
    for label, files, flags in (("compile_stdlib_check", std, ["--check", "--module"]),
                                ("compile_showcase", show, ["-O2"])):
        walls = []
        for _ in range(runs):
            total = 0.0
            for f in files:
                cmd = [turfc, f] + flags
                if "-O2" in flags:
                    cmd += ["-o", os.path.join(work, "showcase_bin")]
                w, _, text, code = run_timed(cmd, cwd=work)
                if code != 0:
                    return dict(out, **{label: {"error": "%s failed: %s" % (
                        os.path.relpath(f, ROOT), text.strip()[-200:])}})
                total += w
            walls.append(total)
        out[label] = summarize(walls)
        out[label]["files"] = len(files)
    return out


def table(results):
    rows = ["| Benchmark | Median (s) | Min (s) | Peak RSS (KB) | vs C | Checksum |",
            "|---|---|---|---|---|---|"]
    for name, r in results.items():
        if "error" in r and "median_s" not in r:
            rows.append("| %s | error: %s | | | | |" % (name, r["error"]))
            continue
        rows.append("| %s | %.3f | %.3f | %s | %s | %s |" % (
            name, r["median_s"], r["min_s"], r.get("rss_kb") or "",
            ("%.2fx" % r["ratio_vs_c"]) if "ratio_vs_c" in r else "",
            r.get("checksum") or ("%d files" % r["files"] if "files" in r else "")))
    return "\n".join(rows)


def compare(results, base, threshold):
    bad = []
    for name, r in results.items():
        b = base["results"].get(name)
        if not b or "median_s" not in r or "median_s" not in b:
            continue
        change = r["median_s"] / b["median_s"] - 1.0
        mark = "REGRESSION" if change > threshold else "ok"
        print("  %-22s %8.3f s -> %8.3f s  %+6.1f%%  %s" % (
            name, b["median_s"], r["median_s"], change * 100, mark))
        if change > threshold:
            bad.append(name)
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--turfc", default=os.path.join(ROOT, "build_release", "turfc"),
                    help="compiler to measure (default build_release/turfc)")
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--only", help="only benchmarks whose name contains this")
    ap.add_argument("--out", help="write the results as JSON here")
    ap.add_argument("--compare", help="a previous --out file: exit 1 on a regression")
    ap.add_argument("--threshold", type=float, default=0.20)
    a = ap.parse_args()
    a.turfc = os.path.abspath(a.turfc)  # compiles run inside a temp directory
    if not os.access(a.turfc, os.X_OK):
        sys.exit("no compiler at %s (build build_release, or pass --turfc)" % a.turfc)

    info = meta(a.turfc)
    if info["build_type"] not in ("Release", "RelWithDebInfo"):
        print("warning: %s is a %s build; compile-time numbers are not comparable "
              "to a Release build's" % (info["turfc"], info["build_type"]), file=sys.stderr)
    results = {}
    with tempfile.TemporaryDirectory(prefix="turf_bench_") as work:
        for src in sorted(os.listdir(BENCH)):
            if not src.endswith(".tr"):
                continue
            name = src[:-3]
            if a.only and a.only not in name:
                continue
            print("== %s" % name, flush=True)
            results[name] = bench_program(a.turfc, os.path.join(BENCH, src), a.runs, work)
        if not a.only or "compile" in a.only:
            print("== compile time", flush=True)
            results.update(bench_compile(a.turfc, a.runs, work))

    doc = {"meta": info, "runs": a.runs, "results": results}
    if a.out:
        with open(a.out, "w") as f:
            json.dump(doc, f, indent=2)
            f.write("\n")
    m = info["machine"]
    print("\n%s, commit %s, %s build, %s (%s cores), %s\n" % (
        info["date"], info["commit"], info["build_type"], m["cpu"], m["cores"], m["os"]))
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
