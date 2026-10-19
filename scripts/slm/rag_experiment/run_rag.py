#!/usr/bin/env python3
"""Run the RAG experiment: for each (case, arm) ask the local model for a
one-line fix, apply it, compile, run, score, append to results/<run>.jsonl.
Resumable; arms are interleaved per case. See README.md."""
from __future__ import annotations

import argparse
import atexit
import hashlib
import json
import os
import platform
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rag_lib as L  # noqa: E402

HERE = Path(__file__).resolve().parent
REPO = L.REPO_ROOT


def load_jsonl(p: Path) -> list[dict]:
    return [json.loads(x) for x in p.read_text().splitlines() if x.strip()]


def sha256(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 22), b""):
            h.update(b)
    return h.hexdigest()


def start_server(a) -> tuple[subprocess.Popen | None, str]:
    if a.server_url:
        return None, a.server_url
    port = a.port
    cmd = [a.llama_server, "-m", a.model, "-t", str(a.threads), "-c", "4096", "-np", "1",
           "--temp", "0", "--seed", "0", "--port", str(port), "--host", "127.0.0.1",
           "-ngl", str(a.gpu_layers)]
    log = open(Path(a.scratch) / "llama-server.log", "w")
    proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT)
    atexit.register(lambda: proc.poll() is None and (proc.terminate(), proc.wait(30)))
    url = f"http://127.0.0.1:{port}"
    for _ in range(600):
        if proc.poll() is not None:
            sys.exit(f"llama-server exited early, see {log.name}")
        try:
            with urllib.request.urlopen(url + "/health", timeout=2) as r:
                if json.loads(r.read()).get("status") == "ok":
                    return proc, url
        except Exception:
            pass
        time.sleep(1)
    sys.exit("llama-server did not become healthy")


def run(cmd, timeout, cwd=None):
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=timeout, cwd=cwd)
        return p.returncode, p.stdout.decode("utf-8", "replace"), False
    except subprocess.TimeoutExpired:
        return -1, "", True


def score(a, case: dict, out: dict, scratch: Path) -> dict:
    res = dict(parsed=False, abstained=False, right_line=False, applied=False,
               compiles=False, correct=False, exact=False, line=None, new_code=None)
    try:
        obj = json.loads(out["raw"])
        line, nc = obj["line"], obj["new_code"]
        assert isinstance(line, int) and not isinstance(line, bool) and isinstance(nc, str)
    except Exception:
        return res
    res.update(parsed=True, line=line, new_code=nc)
    if line == 0:
        res["abstained"] = True
        return res
    res["right_line"] = line == case["line"]
    res["exact"] = nc.strip() == case["original_line"].strip()
    src = case["mutated_source"].split("\n")
    if "\n" in nc or "\r" in nc or not (1 <= line <= len(src)):
        return res
    src[line - 1] = nc
    res["applied"] = True
    f = scratch / f"{case['id']}__{out['arm']}.tr"
    exe = f.with_suffix(".bin")
    f.write_text("\n".join(src))
    rc, _, _ = run([a.turfc, str(f), "-o", str(exe)], 120, cwd=REPO)
    res["compiles"] = rc == 0 and exe.exists()
    if res["compiles"]:
        try:
            p = subprocess.run([str(exe)], capture_output=True, timeout=10)
            res["correct"] = (p.returncode == case["expected_exit"] and
                              p.stdout.decode("utf-8", "replace") == case["expected_stdout"])
        except subprocess.TimeoutExpired:
            pass
    for x in (f, exe):
        try:
            x.unlink()
        except OSError:
            pass
    return res


def selftest(server) -> None:
    """The leakage assertion must fire on crafted bad cases."""
    case = {"id": "q", "source_test": "t/a.tr", "split": "test"}
    good = {"id": "g", "source_test": "t/b.tr", "split": "train"}
    L.assert_no_leak(case, [good])
    for bad in ({**good, "source_test": "t/a.tr"}, {**good, "split": "test"}, {**good, "id": "q"}):
        try:
            L.assert_no_leak(case, [bad])
        except AssertionError as e:
            print("leak assertion fired:", e)
        else:
            sys.exit("SELFTEST FAILED: bad example was not rejected")
    print("selftest ok")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus-dir", default=str(HERE / "corpus"))
    ap.add_argument("--split", default="test")
    ap.add_argument("--arms", default=",".join(L.ARMS))
    ap.add_argument("--limit", type=int)
    ap.add_argument("--run-name", default="run")
    ap.add_argument("--results-dir", default=str(HERE / "results"))
    ap.add_argument("--server-url")
    ap.add_argument("--llama-server", default=str(REPO / "external/llama.cpp/build/bin/llama-server"))
    ap.add_argument("--model", default=str(REPO / "models/qwen2.5-coder-1.5b-instruct-q4_k_m.gguf"))
    ap.add_argument("--turfc", default=str(REPO / "build/turfc"))
    ap.add_argument("--threads", type=int, default=8)
    ap.add_argument("--gpu-layers", type=int, default=0)
    ap.add_argument("--port", type=int, default=8089)
    ap.add_argument("--budget", type=int, default=L.DEFAULT_BUDGET)
    ap.add_argument("--scratch", default=os.path.join(tempfile.gettempdir(), "rag_runs"))
    ap.add_argument("--cache-prompt", action="store_true",
                    help="reuse the KV cache of the shared prompt prefix (faster, tiny numeric drift)")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--dump", action="store_true", help="print each prompt's context and exit after --limit cases without scoring")
    a = ap.parse_args()
    os.environ.pop("TURF_HOME", None)
    arms = a.arms.split(",")
    for x in arms:
        assert x in L.ARMS, f"unknown arm {x}"
    scratch = Path(a.scratch)
    scratch.mkdir(parents=True, exist_ok=True)

    cdir = Path(a.corpus_dir)
    train = load_jsonl(cdir / "train.jsonl")
    cases = load_jsonl(cdir / f"{a.split}.jsonl")
    assert all(c["split"] == a.split for c in cases)
    if a.limit:
        cases = cases[: a.limit]

    proc, url = start_server(a)
    server = L.Server(url)
    try:
        if a.selftest:
            selftest(server)
            return 0
        retr = L.Retriever(server, train, a.budget)
        rdir = Path(a.results_dir)
        rdir.mkdir(parents=True, exist_ok=True)
        rfile = rdir / f"{a.run_name}.jsonl"
        meta = rdir / f"{a.run_name}.meta.json"
        done = set()
        if rfile.exists():
            done = {(r["id"], r["arm"]) for r in load_jsonl(rfile)}
        info = {"platform": platform.platform(), "machine": platform.machine(),
                "python": platform.python_version(),
                "model": os.path.basename(a.model),
                "model_sha256": sha256(Path(a.model)) if Path(a.model).exists() else None,
                "llama_server_version": None, "gpu_layers": a.gpu_layers,
                "threads": a.threads, "budget": a.budget, "arms": arms}
        try:
            v = subprocess.run([a.llama_server, "--version"], capture_output=True, text=True, timeout=30)
            info["llama_server_version"] = (v.stdout + v.stderr).strip().split("\n")[0]
        except Exception:
            pass
        if a.server_url:
            info["server_url"] = a.server_url
        if meta.exists():
            old = json.loads(meta.read_text())
            keys = ("platform", "machine", "model_sha256", "llama_server_version")
            diff = [k for k in keys if old.get(k) != info.get(k)]
            if diff:
                sys.exit(f"run {a.run_name} was started with a different {diff}; use a new --run-name")
        else:
            meta.write_text(json.dumps(info, indent=1))

        total = len(cases) * len(arms)
        n = 0
        with open(rfile, "a") as out:
            for case in cases:
                for arm in arms:
                    n += 1
                    if (case["id"], arm) in done:
                        continue
                    ctx, rinfo = retr.context(arm, case)
                    ctx_tokens = server.count(ctx) if ctx else 0
                    assert ctx_tokens <= a.budget, (arm, ctx_tokens)
                    user, trunc = L.build_user_prompt(server, case, ctx)
                    if a.dump:
                        print(f"=== {case['id']} / {arm} ({ctx_tokens} ctx tokens) {rinfo}\n{ctx}\n")
                        continue
                    t0 = time.monotonic()
                    resp = server.chat(L.SYSTEM_PROMPT, user, L.FIX_SCHEMA, cache_prompt=a.cache_prompt)
                    dt = time.monotonic() - t0
                    raw = resp["choices"][0]["message"]["content"]
                    rec = {"id": case["id"], "arm": arm, "kind": case.get("kind"),
                           "error_code": case.get("error_code"), "split": case["split"],
                           "prompt_tokens": resp["usage"]["prompt_tokens"],
                           "ctx_tokens": ctx_tokens, "latency_s": round(dt, 3),
                           "program_truncated": trunc, "retrieved": rinfo, "raw": raw}
                    rec.update(score(a, case, {"raw": raw, "arm": arm}, scratch))
                    out.write(json.dumps(rec) + "\n")
                    out.flush()
                    print(f"[{n}/{total}] {case['id']:<32} {arm:<20} {dt:6.1f}s "
                          f"ctx={ctx_tokens:<5} correct={rec['correct']} compiles={rec['compiles']}",
                          flush=True)
    finally:
        if proc and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(30)
            except subprocess.TimeoutExpired:
                proc.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
